// CryptoOrderEntry.cpp — production order ticket.
//
// Layout (top → bottom):
//   1. Header strip       — title + paper/live badge
//   2. Account card       — Balance | Mark | Avail-after-order
//   3. BUY / SELL tabs    — full-width, color-coded
//   4. Type buttons       — Market / Limit / Stop / Stop-Limit
//   5. Quantity input     — large monospace, with unit suffix and 25/50/75/100 quick fills
//   6. Price + Stop rows  — auto-shown only for the order types that need them
//   7. Order breakdown    — Cost / Fee / Total / Receive / % of balance
//   8. Advanced (collapsible) — SL / TP
//   9. Futures (visible only when set_futures_mode(true)) — leverage spin + margin segmented control
//  10. Submit             — color-coded big button with computed subtitle
//  11. Status              — inline validation message
//
// Sizing principles:
//  • No fixed pixel heights on inputs/buttons — everything uses padding via QSS so
//    rows scale with the user's terminal font size and HiDPI multipliers.
//  • Right panel is ~290 px wide; layout is single-column with paired label/value
//    sub-rows for compact cost breakdown.
//  • Live cost preview runs on every keystroke (qty / price / type changes) and
//    feeds the submit-button subtitle so the user never has to compute anything.

#include "screens/crypto_trading/CryptoOrderEntry.h"

#include "storage/repositories/SettingsRepository.h"

#include <QComboBox>
#include <QDoubleValidator>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::screens::crypto {

namespace {

QFrame* make_card(const QString& object_name = "cryptoOeCard") {
    auto* card = new QFrame;
    card->setObjectName(object_name);
    card->setFrameShape(QFrame::NoFrame);
    return card;
}

// One label-value row for the account/breakdown cards. Label is upper-cased
// dim text; value is bright primary. Returns the value label so callers can
// update it later.
QLabel* add_kv_row(QGridLayout* grid, int row, const QString& label_text,
                   const QString& value_obj_name = "cryptoOeKvValue") {
    auto* lbl = new QLabel(label_text);
    lbl->setObjectName("cryptoOeKvLabel");
    auto* val = new QLabel(QStringLiteral("--"));
    val->setObjectName(value_obj_name);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    grid->addWidget(lbl, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(val, row, 1, Qt::AlignRight | Qt::AlignVCenter);
    return val;
}

// Base symbol of a "BASE/QUOTE" pair, defaulting to the whole string if no slash.
QString base_of(const QString& pair) {
    const int slash = pair.indexOf('/');
    return slash > 0 ? pair.left(slash) : pair;
}

// Quote symbol (typically USDT, USD, USDC, …) of a pair.
QString quote_of(const QString& pair) {
    const int slash = pair.indexOf('/');
    return slash > 0 ? pair.mid(slash + 1) : QStringLiteral("USD");
}

// Numeric-only validator that allows decimals; reused by every numeric input.
QDoubleValidator* make_decimal_validator(QObject* parent) {
    auto* v = new QDoubleValidator(0.0, 1e15, 8, parent);
    v->setNotation(QDoubleValidator::StandardNotation);
    v->setLocale(QLocale::c());
    return v;
}

void repolish(QWidget* w) {
    if (!w || !w->style()) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
}

struct FeeSchedule {
    double maker = 0.0010;
    double taker = 0.0015;
    double rebate_pct = 0.0;
    double free_remaining_usd = 0.0;
    bool free_applies_to_advanced = false;
    double slippage = 0.0;
    QString label = QStringLiteral("venue est");
    QString note;
};

QString fee_venue_key(QString exchange_id) {
    exchange_id = exchange_id.trimmed().toLower();
    if (exchange_id.contains(QStringLiteral("coinbase")))
        return QStringLiteral("coinbase");
    if (exchange_id.contains(QStringLiteral("alpaca")))
        return QStringLiteral("alpaca");
    if (exchange_id.contains(QStringLiteral("kraken")))
        return QStringLiteral("kraken");
    if (exchange_id.contains(QStringLiteral("binance")))
        return QStringLiteral("binance");
    return exchange_id.isEmpty() ? QStringLiteral("default") : exchange_id;
}

QString fee_setting_key(const QString& venue_key, const QString& field) {
    return QStringLiteral("crypto.fee.%1.%2").arg(venue_key, field);
}

QString fee_setting_string(const QString& key, const QString& fallback = {}) {
    auto r = SettingsRepository::instance().get(key, fallback);
    if (r.is_err())
        return fallback;
    const QString value = r.value().trimmed();
    return value.isEmpty() ? fallback : value;
}

double fee_setting_double(const QString& key, double fallback) {
    bool ok = false;
    const double value = fee_setting_string(key, QString::number(fallback, 'g', 12)).toDouble(&ok);
    return ok ? value : fallback;
}

bool fee_setting_bool(const QString& key, bool fallback) {
    const QString raw = fee_setting_string(key, fallback ? QStringLiteral("true") : QStringLiteral("false")).toLower();
    if (raw == QLatin1String("true") || raw == QLatin1String("yes") || raw == QLatin1String("1") ||
        raw == QLatin1String("on"))
        return true;
    if (raw == QLatin1String("false") || raw == QLatin1String("no") || raw == QLatin1String("0") ||
        raw == QLatin1String("off"))
        return false;
    return fallback;
}

FeeSchedule fee_schedule_for(QString exchange_id) {
    exchange_id = exchange_id.trimmed().toLower();
    FeeSchedule schedule;
    if (exchange_id == QLatin1String("coinbase")) {
        // Conservative Coinbase Advanced base-tier estimate for small accounts.
        // The account-specific tier can be lower; keep this pessimistic until
        // we read live fee tier data from the authenticated account.
        schedule = {0.0060, 0.0120, 0.0, 0.0, false, 0.0,
                    QStringLiteral("Coinbase Advanced est"),
                    QStringLiteral("Coinbase One simple-trade fee perks are not assumed for Advanced/API orders.")};
    } else if (exchange_id == QLatin1String("alpaca")) {
        schedule = {0.0015, 0.0025, 0.0, 0.0, false, 0.0, QStringLiteral("Alpaca tier 1"), QString()};
    } else if (exchange_id == QLatin1String("kraken")) {
        schedule = {0.0025, 0.0040, 0.0, 0.0, false, 0.0, QStringLiteral("Kraken starter"), QString()};
    } else if (exchange_id == QLatin1String("binance")) {
        schedule = {0.0010, 0.0010, 0.0, 0.0, false, 0.0, QStringLiteral("Binance starter"), QString()};
    } else {
        schedule = {0.0010, 0.0015, 0.0, 0.0, false, 0.0, QStringLiteral("default est"), QString()};
    }

    const QString venue_key = fee_venue_key(exchange_id);
    const QString profile = fee_setting_string(fee_setting_key(venue_key, QStringLiteral("profile")));
    schedule.maker = fee_setting_double(fee_setting_key(venue_key, QStringLiteral("maker_bps")),
                                        schedule.maker * 10000.0) / 10000.0;
    schedule.taker = fee_setting_double(fee_setting_key(venue_key, QStringLiteral("taker_bps")),
                                        schedule.taker * 10000.0) / 10000.0;
    schedule.rebate_pct = std::clamp(fee_setting_double(fee_setting_key(venue_key, QStringLiteral("rebate_pct")),
                                                        schedule.rebate_pct), 0.0, 100.0);
    schedule.free_remaining_usd =
        std::max(0.0, fee_setting_double(fee_setting_key(venue_key, QStringLiteral("free_remaining_usd")),
                                         schedule.free_remaining_usd));
    schedule.free_applies_to_advanced =
        fee_setting_bool(fee_setting_key(venue_key, QStringLiteral("free_applies_to_advanced")),
                         schedule.free_applies_to_advanced);
    schedule.slippage = std::max(0.0, fee_setting_double(fee_setting_key(venue_key, QStringLiteral("slippage_bps")),
                                                        schedule.slippage * 10000.0)) / 10000.0;
    const QString note = fee_setting_string(fee_setting_key(venue_key, QStringLiteral("note")));
    if (!note.isEmpty())
        schedule.note = note;
    if (!profile.isEmpty())
        schedule.label = profile + QStringLiteral(" local");
    return schedule;
}

} // namespace

CryptoOrderEntry::CryptoOrderEntry(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoOrderEntry");
    setMinimumWidth(240);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ──────────────────────────────────────────────────────────────
    auto* header = new QWidget(this);
    header->setObjectName("cryptoOeHeader");
    auto* h_layout = new QHBoxLayout(header);
    h_layout->setContentsMargins(12, 6, 12, 6);
    h_layout->setSpacing(6);

    title_label_ = new QLabel(tr("ORDER ENTRY"));
    title_label_->setObjectName("cryptoOeTitle");
    h_layout->addWidget(title_label_);
    h_layout->addStretch();

    mode_label_ = new QLabel(tr("PAPER"));
    mode_label_->setObjectName("cryptoOeMode");
    mode_label_->setProperty("mode", "paper");
    h_layout->addWidget(mode_label_);

    root->addWidget(header);

    // ── Scrollable content ──────────────────────────────────────────────────
    auto* content = new QWidget(this);
    auto* form = new QVBoxLayout(content);
    form->setContentsMargins(10, 10, 10, 10);
    form->setSpacing(10);

    // ── 1. Account card (Balance / Mark / Avail) ────────────────────────────
    {
        auto* card = make_card();
        auto* grid = new QGridLayout(card);
        grid->setContentsMargins(12, 10, 12, 10);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(0, 0);
        grid->setColumnStretch(1, 1);

        balance_label_      = add_kv_row(grid, 0, tr("BALANCE"),  "cryptoOeKvValueAccent");
        market_price_label_ = add_kv_row(grid, 1, tr("MARK"));
        avail_label_        = add_kv_row(grid, 2, tr("AVAIL"),    "cryptoOeKvValueDim");
        balance_title_ = qobject_cast<QLabel*>(grid->itemAtPosition(0, 0)->widget());
        mark_title_    = qobject_cast<QLabel*>(grid->itemAtPosition(1, 0)->widget());
        avail_title_   = qobject_cast<QLabel*>(grid->itemAtPosition(2, 0)->widget());

        balance_label_->setText(QStringLiteral("$0.00"));
        market_price_label_->setText(QStringLiteral("--"));
        avail_label_->setText(QStringLiteral("--"));

        form->addWidget(card);
    }

    // ── 2. BUY / SELL tabs ──────────────────────────────────────────────────
    {
        auto* side_row = new QHBoxLayout;
        side_row->setSpacing(0);
        side_row->setContentsMargins(0, 0, 0, 0);

        buy_tab_ = new QPushButton(tr("BUY"));
        buy_tab_->setObjectName("cryptoBuyTab");
        buy_tab_->setProperty("active", true);
        buy_tab_->setCursor(Qt::PointingHandCursor);
        buy_tab_->setFocusPolicy(Qt::NoFocus);
        connect(buy_tab_, &QPushButton::clicked, this, [this]() { set_buy_side(true); });
        side_row->addWidget(buy_tab_, 1);

        sell_tab_ = new QPushButton(tr("SELL"));
        sell_tab_->setObjectName("cryptoSellTab");
        sell_tab_->setProperty("active", false);
        sell_tab_->setCursor(Qt::PointingHandCursor);
        sell_tab_->setFocusPolicy(Qt::NoFocus);
        connect(sell_tab_, &QPushButton::clicked, this, [this]() { set_buy_side(false); });
        side_row->addWidget(sell_tab_, 1);

        form->addLayout(side_row);
    }

    // ── 3. Order type segmented control ─────────────────────────────────────
    {
        auto* type_row = new QHBoxLayout;
        type_row->setSpacing(0);
        type_row->setContentsMargins(0, 0, 0, 0);
        // Slightly more readable than the previous 3-letter abbreviations.
        const QString type_labels[] = {tr("MARKET"), tr("LIMIT"), tr("STOP"), tr("STOP-LMT")};
        for (int i = 0; i < 4; ++i) {
            type_btns_[i] = new QPushButton(type_labels[i]);
            type_btns_[i]->setObjectName("cryptoOeTypeBtn");
            type_btns_[i]->setCursor(Qt::PointingHandCursor);
            type_btns_[i]->setFocusPolicy(Qt::NoFocus);
            if (i == 0) type_btns_[i]->setProperty("active", true);
            connect(type_btns_[i], &QPushButton::clicked, this, [this, i]() { set_order_type(i); });
            type_row->addWidget(type_btns_[i], 1);
        }
        form->addLayout(type_row);
    }

    // ── 4. Quantity input + 25/50/75/100% quick fills ───────────────────────
    {
        qty_title_ = new QLabel(tr("QUANTITY"));
        qty_title_->setObjectName("cryptoOeLabel");
        form->addWidget(qty_title_);

        // Quantity field with the base-asset suffix shown in a side label so
        // users always know whether they're sizing in BTC or USDT.
        auto* qty_wrap = new QWidget;
        qty_wrap->setObjectName("cryptoOeInputWrap");
        auto* qty_h = new QHBoxLayout(qty_wrap);
        qty_h->setContentsMargins(0, 0, 0, 0);
        qty_h->setSpacing(0);

        qty_edit_ = new QLineEdit;
        qty_edit_->setObjectName("cryptoOeInput");
        qty_edit_->setPlaceholderText(QStringLiteral("0.00"));
        qty_edit_->setValidator(make_decimal_validator(this));
        qty_edit_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(qty_edit_, &QLineEdit::textChanged, this, [this]() { update_cost_preview(); });
        qty_h->addWidget(qty_edit_, 1);

        auto* unit = new QLabel(base_of(current_symbol_));
        unit->setObjectName("cryptoOeUnit");
        unit->setMinimumWidth(40);
        unit->setAlignment(Qt::AlignCenter);
        qty_h->addWidget(unit);

        form->addWidget(qty_wrap);

        // Quick % fills — bigger touch targets than the old 18 px buttons.
        auto* pct_row = new QHBoxLayout;
        pct_row->setSpacing(4);
        for (int pct : {25, 50, 75, 100}) {
            auto* btn = new QPushButton(QString::number(pct) + QStringLiteral("%"));
            btn->setObjectName("cryptoOePctBtn");
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFocusPolicy(Qt::NoFocus);
            connect(btn, &QPushButton::clicked, this, [this, pct]() { on_pct_clicked(pct); });
            pct_row->addWidget(btn, 1);
        }
        form->addLayout(pct_row);

        // Symbol-change signal will update the unit label.
        connect(this, &CryptoOrderEntry::destroyed, unit, [](){});
        // Repurpose set_symbol's path: we keep a reference to update later.
        unit->setProperty("ftRoleUnit", true);
    }

    // ── 5. Price (Limit / Stop-Limit) ───────────────────────────────────────
    {
        price_row_ = new QWidget;
        auto* v = new QVBoxLayout(price_row_);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        price_title_ = new QLabel(tr("LIMIT PRICE"));
        price_title_->setObjectName("cryptoOeLabel");
        v->addWidget(price_title_);

        auto* wrap = new QWidget;
        auto* h = new QHBoxLayout(wrap);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        price_edit_ = new QLineEdit;
        price_edit_->setObjectName("cryptoOeInput");
        price_edit_->setPlaceholderText(QStringLiteral("0.00"));
        price_edit_->setValidator(make_decimal_validator(this));
        price_edit_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(price_edit_, &QLineEdit::textChanged, this, [this]() { update_cost_preview(); });
        h->addWidget(price_edit_, 1);

        auto* unit = new QLabel(quote_of(current_symbol_));
        unit->setObjectName("cryptoOeUnit");
        unit->setMinimumWidth(40);
        unit->setAlignment(Qt::AlignCenter);
        unit->setProperty("ftRoleUnit", "quote");
        h->addWidget(unit);

        v->addWidget(wrap);
        form->addWidget(price_row_);
        price_row_->setVisible(false); // shown only when type is Limit / Stop-Limit
    }

    post_only_check_ = new QCheckBox(tr("POST ONLY"));
    post_only_check_->setObjectName("cryptoOeCheck");
    post_only_check_->setCursor(Qt::PointingHandCursor);
    post_only_check_->setToolTip(tr("Maker protection: the exchange should reject/cancel the order if it would fill immediately."));
    post_only_check_->setVisible(false);
    connect(post_only_check_, &QCheckBox::toggled, this, [this]() { update_cost_preview(); });
    form->addWidget(post_only_check_);

    // ── 6. Stop trigger price (Stop / Stop-Limit) ───────────────────────────
    {
        stop_row_ = new QWidget;
        auto* v = new QVBoxLayout(stop_row_);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        stop_title_ = new QLabel(tr("STOP PRICE"));
        stop_title_->setObjectName("cryptoOeLabel");
        v->addWidget(stop_title_);

        auto* wrap = new QWidget;
        auto* h = new QHBoxLayout(wrap);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        stop_price_edit_ = new QLineEdit;
        stop_price_edit_->setObjectName("cryptoOeInput");
        stop_price_edit_->setPlaceholderText(QStringLiteral("0.00"));
        stop_price_edit_->setValidator(make_decimal_validator(this));
        stop_price_edit_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(stop_price_edit_, 1);

        auto* unit = new QLabel(quote_of(current_symbol_));
        unit->setObjectName("cryptoOeUnit");
        unit->setMinimumWidth(40);
        unit->setAlignment(Qt::AlignCenter);
        unit->setProperty("ftRoleUnit", "quote");
        h->addWidget(unit);

        v->addWidget(wrap);
        form->addWidget(stop_row_);
        stop_row_->setVisible(false);
    }

    // ── 7. Order breakdown card ────────────────────────────────────────────
    {
        auto* card = make_card("cryptoOeBreakdown");
        auto* grid = new QGridLayout(card);
        grid->setContentsMargins(12, 8, 12, 8);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(0, 0);
        grid->setColumnStretch(1, 1);

        cost_label_     = add_kv_row(grid, 0, tr("ORDER VALUE"));
        fee_label_      = add_kv_row(grid, 1, tr("COST EST"),    "cryptoOeKvValueDim");
        total_label_    = add_kv_row(grid, 2, tr("TOTAL"),       "cryptoOeKvValueAccent");
        recv_label_     = add_kv_row(grid, 3, tr("YOU RECEIVE"), "cryptoOeKvValueDim");
        pct_used_label_ = add_kv_row(grid, 4, tr("BREAKEVEN"),   "cryptoOeKvValueDim");
        cost_title_     = qobject_cast<QLabel*>(grid->itemAtPosition(0, 0)->widget());
        fee_title_      = qobject_cast<QLabel*>(grid->itemAtPosition(1, 0)->widget());
        total_title_    = qobject_cast<QLabel*>(grid->itemAtPosition(2, 0)->widget());
        recv_title_     = qobject_cast<QLabel*>(grid->itemAtPosition(3, 0)->widget());
        pct_used_title_ = qobject_cast<QLabel*>(grid->itemAtPosition(4, 0)->widget());

        form->addWidget(card);
    }

    // ── 8. Advanced toggle (SL / TP) ────────────────────────────────────────
    advanced_toggle_ = new QPushButton(tr("▾  Advanced  (SL / TP)"));
    advanced_toggle_->setObjectName("cryptoAdvToggle");
    advanced_toggle_->setCursor(Qt::PointingHandCursor);
    advanced_toggle_->setFocusPolicy(Qt::NoFocus);
    form->addWidget(advanced_toggle_);

    advanced_section_ = new QWidget(this);
    advanced_section_->setVisible(false);
    {
        auto* adv = new QVBoxLayout(advanced_section_);
        adv->setContentsMargins(0, 0, 0, 0);
        adv->setSpacing(6);

        auto add_pair = [&](const QString& label, QLineEdit*& edit, QLabel*& title_out,
                            const QString& placeholder) {
            auto* lbl = new QLabel(label);
            lbl->setObjectName("cryptoOeLabel");
            adv->addWidget(lbl);
            title_out = lbl;
            edit = new QLineEdit;
            edit->setObjectName("cryptoOeInput");
            edit->setPlaceholderText(placeholder);
            edit->setValidator(make_decimal_validator(this));
            edit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            adv->addWidget(edit);
        };
        add_pair(tr("STOP LOSS"),   sl_edit_, sl_title_, tr("Trigger price"));
        add_pair(tr("TAKE PROFIT"), tp_edit_, tp_title_, tr("Trigger price"));
    }
    form->addWidget(advanced_section_);

    connect(advanced_toggle_, &QPushButton::clicked, this, [this]() {
        const bool show = !advanced_section_->isVisible();
        advanced_section_->setVisible(show);
        advanced_toggle_->setText(show ? tr("▴  Advanced  (SL / TP)")
                                       : tr("▾  Advanced  (SL / TP)"));
    });

    // ── 9. Futures controls (leverage + margin mode) ────────────────────────
    futures_section_ = new QWidget(this);
    futures_section_->setVisible(false);
    {
        auto* layout = new QVBoxLayout(futures_section_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        leverage_title_ = new QLabel(tr("LEVERAGE"));
        leverage_title_->setObjectName("cryptoOeLabel");
        layout->addWidget(leverage_title_);

        leverage_spin_ = new QSpinBox;
        leverage_spin_->setObjectName("cryptoOeSpinBox");
        leverage_spin_->setRange(1, 125);
        leverage_spin_->setValue(1);
        leverage_spin_->setSuffix(QStringLiteral("x"));
        leverage_spin_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(leverage_spin_);

        margin_title_ = new QLabel(tr("MARGIN MODE"));
        margin_title_->setObjectName("cryptoOeLabel");
        layout->addWidget(margin_title_);

        margin_mode_combo_ = new QComboBox;
        margin_mode_combo_->setObjectName("cryptoOeCombo");
        margin_mode_combo_->addItem(tr("Cross"),    QStringLiteral("cross"));
        margin_mode_combo_->addItem(tr("Isolated"), QStringLiteral("isolated"));
        layout->addWidget(margin_mode_combo_);

        reduce_only_check_ = new QCheckBox(tr("Reduce only"));
        reduce_only_check_->setObjectName("cryptoOeCheck");
        layout->addWidget(reduce_only_check_);
    }
    form->addWidget(futures_section_);

    connect(leverage_spin_, &QSpinBox::valueChanged, this, [this](int val) {
        emit leverage_changed(val);
        update_cost_preview();
    });
    connect(margin_mode_combo_, &QComboBox::currentIndexChanged, this,
            [this](int idx) { emit margin_mode_changed(margin_mode_combo_->itemData(idx).toString()); });

    // ── 10. Submit button + computed subtitle ───────────────────────────────
    submit_btn_ = new QPushButton(tr("BUY  %1").arg(current_symbol_));
    submit_btn_->setObjectName("cryptoBuySubmit");
    submit_btn_->setCursor(Qt::PointingHandCursor);
    submit_btn_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    submit_btn_->setToolTip(tr("Submit order  (⌘/Ctrl+Enter)"));
    connect(submit_btn_, &QPushButton::clicked, this, &CryptoOrderEntry::on_submit);
    form->addWidget(submit_btn_);

    submit_subtitle_ = new QLabel(tr("Enter a quantity to preview"));
    submit_subtitle_->setObjectName("cryptoOeSubmitSubtitle");
    submit_subtitle_->setAlignment(Qt::AlignCenter);
    submit_subtitle_->setWordWrap(true);
    form->addWidget(submit_subtitle_);

    // ── 11. Inline status / validation message ──────────────────────────────
    status_label_ = new QLabel;
    status_label_->setObjectName("cryptoOeStatus");
    status_label_->setWordWrap(true);
    status_label_->setVisible(false);
    form->addWidget(status_label_);

    form->addStretch();
    root->addWidget(content, 1);

    update_cost_preview();
}

void CryptoOrderEntry::set_buy_side(bool is_buy) {
    if (is_buy == is_buy_side_)
        return;
    is_buy_side_ = is_buy;
    buy_tab_->setProperty("active", is_buy);
    sell_tab_->setProperty("active", !is_buy);

    if (!submit_busy_)
        submit_btn_->setText(is_buy ? tr("BUY  %1").arg(current_symbol_) : tr("SELL  %1").arg(current_symbol_));
    submit_btn_->setObjectName(is_buy ? "cryptoBuySubmit" : "cryptoSellSubmit");

    repolish(buy_tab_);
    repolish(sell_tab_);
    repolish(submit_btn_);
    update_cost_preview();
}

void CryptoOrderEntry::set_order_type(int idx) {
    const int prev = active_type_;
    if (idx == active_type_) {
        // Defensive: still recompute visibility / preview in case state drifted.
        if (price_row_) price_row_->setVisible(idx == 1 || idx == 3);
        if (post_only_check_) post_only_check_->setVisible(idx == 1 || idx == 3);
        if (stop_row_)  stop_row_->setVisible(idx == 2 || idx == 3);
        if (price_edit_) price_edit_->setEnabled(idx == 1 || idx == 3);
        if (stop_price_edit_) stop_price_edit_->setEnabled(idx == 2 || idx == 3);
        update_cost_preview();
        return;
    }
    active_type_ = idx;
    for (int i = 0; i < 4; ++i)
        type_btns_[i]->setProperty("active", i == idx);

    if (prev >= 0 && prev < 4) repolish(type_btns_[prev]);
    repolish(type_btns_[idx]);

    if (price_row_) price_row_->setVisible(idx == 1 || idx == 3);
    if (post_only_check_) post_only_check_->setVisible(idx == 1 || idx == 3);
    if (stop_row_)  stop_row_->setVisible(idx == 2 || idx == 3);
    if (price_edit_) price_edit_->setEnabled(idx == 1 || idx == 3);
    if (stop_price_edit_) stop_price_edit_->setEnabled(idx == 2 || idx == 3);

    update_cost_preview();
}

// The quote/cash currency to label money figures with. Prefers the actual
// account balance currency reported by the exchange (e.g. Coinbase settles in
// USD even though the display pair reads BTC/USDT), falling back to the pair's
// quote when no live balance currency is known (paper mode, other exchanges).
QString CryptoOrderEntry::quote_label() const {
    return balance_currency_.isEmpty() ? quote_of(current_symbol_) : balance_currency_;
}

void CryptoOrderEntry::set_balance(double balance, const QString& currency) {
    balance_currency_ = currency.trimmed().toUpper();
    balance_ = balance;
    balance_label_->setText(QString("$%1").arg(balance, 0, 'f', 2));
    update_cost_preview();
}

void CryptoOrderEntry::set_current_price(double price) {
    current_price_ = price;
    market_price_label_->setText(QString("$%1").arg(price, 0, 'f', 2));
    update_cost_preview();
}

void CryptoOrderEntry::set_exchange_id(const QString& exchange_id) {
    exchange_id_ = exchange_id.trimmed().toLower();
    update_cost_preview();
}

void CryptoOrderEntry::set_orderbook_quote(double bid, double ask) {
    best_bid_ = bid > 0 ? bid : 0;
    best_ask_ = ask > 0 ? ask : 0;
    update_cost_preview();
}

void CryptoOrderEntry::set_mode(bool is_paper) {
    is_paper_ = is_paper;
    mode_label_->setText(is_paper ? tr("PAPER") : tr("LIVE"));
    mode_label_->setProperty("mode", is_paper ? "paper" : "live");
    repolish(mode_label_);
}

void CryptoOrderEntry::set_symbol(const QString& symbol) {
    current_symbol_ = symbol;
    best_bid_ = 0;
    best_ask_ = 0;
    if (!submit_busy_)
        submit_btn_->setText(is_buy_side_ ? tr("BUY  %1").arg(symbol) : tr("SELL  %1").arg(symbol));

    // Update unit suffix labels (base on qty, quote on price/stop) by looking
    // up the children we tagged with the ftRoleUnit dynamic property.
    const QString base = base_of(symbol);
    const QString quote = quote_label(); // current_symbol_ updated above
    for (auto* lbl : findChildren<QLabel*>()) {
        const auto role = lbl->property("ftRoleUnit");
        if (!role.isValid()) continue;
        if (role.toString() == QLatin1String("quote"))
            lbl->setText(quote);
        else
            lbl->setText(base);
    }
    update_cost_preview();
}

void CryptoOrderEntry::set_submit_busy(bool busy) {
    submit_busy_ = busy;
    if (!submit_btn_)
        return;
    submit_btn_->setEnabled(!busy);
    if (busy) {
        submit_btn_->setText(tr("SENDING…"));
    } else {
        submit_btn_->setText(is_buy_side_ ? tr("BUY  %1").arg(current_symbol_)
                                          : tr("SELL  %1").arg(current_symbol_));
    }
}

void CryptoOrderEntry::on_submit() {
    // In-flight guard: a live order POST is dispatched to a worker thread and
    // the receiver returns immediately, so without this a double-click or a
    // held Ctrl+Enter would fire a second real exchange order. set_submit_busy
    // disables the button, but the keyboard-shortcut / scripted path can still
    // re-enter the slot, so gate here too.
    if (submit_busy_)
        return;

    const QString side = is_buy_side_ ? "buy" : "sell";
    static const char* type_map[] = {"market", "limit", "stop", "stop_limit"};
    const QString order_type = type_map[active_type_];

    const double qty = qty_edit_->text().toDouble();
    if (qty <= 0) {
        status_label_->setText(tr("⚠ Enter a valid quantity"));
        status_label_->setProperty("severity", "error");
        status_label_->setVisible(true);
        repolish(status_label_);
        qty_edit_->setProperty("invalid", true);
        repolish(qty_edit_);
        qty_edit_->setFocus();
        return;
    }
    qty_edit_->setProperty("invalid", false);
    repolish(qty_edit_);

    if ((active_type_ == 1 || active_type_ == 3) && price_edit_->text().toDouble() <= 0) {
        status_label_->setText(tr("⚠ Limit price required"));
        status_label_->setProperty("severity", "error");
        status_label_->setVisible(true);
        repolish(status_label_);
        price_edit_->setFocus();
        return;
    }
    if ((active_type_ == 2 || active_type_ == 3) && stop_price_edit_->text().toDouble() <= 0) {
        status_label_->setText(tr("⚠ Stop trigger price required"));
        status_label_->setProperty("severity", "error");
        status_label_->setVisible(true);
        repolish(status_label_);
        stop_price_edit_->setFocus();
        return;
    }

    const bool post_only = post_only_check_ && post_only_check_->isVisible() && post_only_check_->isChecked();
    if (post_only && (active_type_ == 1 || active_type_ == 3) && best_bid_ > 0 && best_ask_ > best_bid_) {
        const double limit_price = price_edit_->text().toDouble();
        const bool would_cross = (is_buy_side_ && limit_price >= best_ask_) || (!is_buy_side_ && limit_price <= best_bid_);
        if (would_cross) {
            status_label_->setText(is_buy_side_
                                       ? tr("⚠ Post-only buy would cross the ask. Lower the limit below best ask.")
                                       : tr("⚠ Post-only sell would cross the bid. Raise the limit above best bid."));
            status_label_->setProperty("severity", "error");
            status_label_->setVisible(true);
            repolish(status_label_);
            price_edit_->setFocus();
            return;
        }
    }

    status_label_->setVisible(false);

    const double price = price_edit_->text().toDouble();
    const double stop_price = stop_price_edit_->text().toDouble();
    const double sl = sl_edit_->text().toDouble();
    const double tp = tp_edit_->text().toDouble();

    emit order_submitted(side, order_type, qty, price, stop_price, sl, tp, post_only);
}

void CryptoOrderEntry::on_pct_clicked(int pct) {
    if (balance_ <= 0 || current_price_ <= 0)
        return;
    // For limit orders use the limit price as the sizing basis if present, so
    // 100% means "use my whole balance at the limit I'd actually fill at" —
    // not at the current mark.
    double basis = current_price_;
    if ((active_type_ == 1 || active_type_ == 3)) {
        const double limit_p = price_edit_->text().toDouble();
        if (limit_p > 0) basis = limit_p;
    }
    const int leverage = (is_futures_ && leverage_spin_) ? leverage_spin_->value() : 1;
    const double max_qty = (balance_ * leverage) / basis;
    const double qty = max_qty * pct / 100.0;
    qty_edit_->setText(QString::number(qty, 'f', 6));
}

void CryptoOrderEntry::set_futures_mode(bool is_futures) {
    is_futures_ = is_futures;
    futures_section_->setVisible(is_futures);
    if (!is_futures && reduce_only_check_)
        reduce_only_check_->setChecked(false); // spot has no reduce-only concept
}

bool CryptoOrderEntry::reduce_only() const {
    return is_futures_ && reduce_only_check_ && reduce_only_check_->isChecked();
}

void CryptoOrderEntry::update_cost_preview() {
    const double qty = qty_edit_ ? qty_edit_->text().toDouble() : 0.0;
    double price = current_price_;
    if (active_type_ == 1 || active_type_ == 3) {
        const double limit_p = price_edit_ ? price_edit_->text().toDouble() : 0.0;
        if (limit_p > 0) price = limit_p;
    }

    const QString quote = quote_label();
    const QString base  = base_of(current_symbol_);
    auto fmt_money = [&](double v) { return QString("%1 %2").arg(v, 0, 'f', 2).arg(quote); };
    auto fmt_base  = [&](double v) { return QString("%1 %2").arg(v, 0, 'f', 6).arg(base); };

    if (qty > 0 && price > 0) {
        const double notional = qty * price;
        const int leverage = (is_futures_ && leverage_spin_) ? leverage_spin_->value() : 1;
        const double margin = notional / std::max(1, leverage);

        const bool has_quote = best_bid_ > 0 && best_ask_ > best_bid_;
        const bool has_limit = (active_type_ == 1 || active_type_ == 3) && price > 0;
        const bool marketable_limit =
            has_limit && has_quote && ((is_buy_side_ && price >= best_ask_) || (!is_buy_side_ && price <= best_bid_));
        const bool post_only = post_only_check_ && post_only_check_->isVisible() && post_only_check_->isChecked();
        const bool taker = !post_only && (active_type_ == 0 || active_type_ == 2 || marketable_limit || !has_limit);
        const FeeSchedule schedule = fee_schedule_for(exchange_id_);
        const double fee_rate = taker ? schedule.taker : schedule.maker;
        const double gross_fee = notional * fee_rate;
        const double rebate = gross_fee * (schedule.rebate_pct / 100.0);
        const double free_allowance =
            schedule.free_applies_to_advanced ? std::min(std::max(0.0, gross_fee - rebate), schedule.free_remaining_usd)
                                              : 0.0;
        const double fee = std::max(0.0, gross_fee - rebate - free_allowance);
        const double half_spread = has_quote ? std::max(0.0, (best_ask_ - best_bid_) / 2.0) : 0.0;
        const double spread_cost = taker ? half_spread * qty : 0.0;
        const double default_slippage =
            taker ? std::max(notional * 0.0001, spread_cost * (active_type_ == 0 ? 0.35 : 0.20)) : 0.0;
        const double slippage_cost = taker ? (schedule.slippage > 0.0 ? notional * schedule.slippage
                                                                      : default_slippage)
                                           : 0.0;
        const double execution_cost = fee + spread_cost + slippage_cost;
        const double round_trip_breakeven_pct = notional > 0 ? (2.0 * execution_cost / notional) * 100.0 : 0.0;
        const double swing3 = 3.0 - round_trip_breakeven_pct;
        const double swing5 = 5.0 - round_trip_breakeven_pct;
        const double swing8 = 8.0 - round_trip_breakeven_pct;
        const double total = is_buy_side_ ? margin + execution_cost : std::max(0.0, margin - execution_cost);
        const double pct = balance_ > 0 ? (margin / balance_) * 100.0 : 0.0;
        const double avail = std::max(0.0, balance_ - total);

        cost_label_->setText(fmt_money(notional));
        fee_label_->setText(QStringLiteral("~%1").arg(fmt_money(execution_cost)));
        QString tooltip = QStringLiteral("%1 %2\nGross fee: %3\nRebate/free: -%4 / -%5\nNet fee: %6\nSpread: %7\nSlippage: %8")
                              .arg(post_only ? QStringLiteral("MAKER / POST ONLY")
                                             : (taker ? QStringLiteral("TAKER") : QStringLiteral("MAKER")),
                                   schedule.label,
                                   fmt_money(gross_fee),
                                   fmt_money(rebate),
                                   fmt_money(free_allowance),
                                   fmt_money(fee),
                                   fmt_money(spread_cost),
                                   fmt_money(slippage_cost));
        tooltip += QStringLiteral("\nRound-trip breakeven: %1%\nSwing lens net after cost: 3%% -> %2%%, 5%% -> %3%%, 8%% -> %4%%")
                       .arg(round_trip_breakeven_pct, 0, 'f', 2)
                       .arg(swing3, 0, 'f', 2)
                       .arg(swing5, 0, 'f', 2)
                       .arg(swing8, 0, 'f', 2);
        if (!schedule.note.isEmpty())
            tooltip += QStringLiteral("\n") + schedule.note;
        if (post_only)
            tooltip += QStringLiteral("\nWill reject/cancel if immediately marketable.");
        fee_label_->setToolTip(tooltip);
        total_label_->setText(fmt_money(total));
        total_label_->setToolTip(is_buy_side_ ? tr("Estimated debit including fee, spread, and slippage")
                                              : tr("Estimated proceeds after fee, spread, and slippage"));
        recv_label_->setText(is_buy_side_ ? fmt_base(qty) : fmt_money(std::max(0.0, qty * price - execution_cost)));
        pct_used_label_->setText(QStringLiteral("RT %1%  ·  5% swing net %2%  ·  bal %3%")
                                     .arg(round_trip_breakeven_pct, 0, 'f', 2)
                                     .arg(swing5, 0, 'f', 2)
                                     .arg(pct, 0, 'f', 2));
        if (avail_label_)
            avail_label_->setText(fmt_money(avail));

        // Submit subtitle reads "0.31 BTC ≈ $25,000" or, for futures,
        // appends the leverage.
        const QString subtitle =
            is_futures_
                ? QString("%1 @ %2x  ≈  %3  ·  RT BE %4%")
                      .arg(fmt_base(qty))
                      .arg(leverage)
                      .arg(fmt_money(margin))
                      .arg(round_trip_breakeven_pct, 0, 'f', 2)
                : QString("%1  ≈  %2  ·  %3 %4  ·  RT BE %5%")
                      .arg(fmt_base(qty), fmt_money(notional),
                           post_only ? QStringLiteral("POST ONLY") : (taker ? QStringLiteral("TAKER") : QStringLiteral("MAKER")),
                           schedule.label)
                      .arg(round_trip_breakeven_pct, 0, 'f', 2);
        submit_subtitle_->setText(subtitle);

        const bool insufficient = !is_futures_ && total > balance_ && balance_ > 0;
        if (insufficient) {
            status_label_->setText(tr("⚠ Insufficient balance for this order"));
            status_label_->setProperty("severity", "warning");
            status_label_->setVisible(true);
            repolish(status_label_);
        } else if (post_only && marketable_limit) {
            status_label_->setText(is_buy_side_
                                       ? tr("⚠ Post-only buy would reject at/above best ask")
                                       : tr("⚠ Post-only sell would reject at/below best bid"));
            status_label_->setProperty("severity", "warning");
            status_label_->setVisible(true);
            repolish(status_label_);
        } else if (status_label_->property("severity").toString() != QLatin1String("error")) {
            status_label_->setVisible(false);
        }
    } else {
        cost_label_->setText(QStringLiteral("--"));
        fee_label_->setText(QStringLiteral("--"));
        fee_label_->setToolTip(QString());
        total_label_->setText(QStringLiteral("--"));
        total_label_->setToolTip(QString());
        recv_label_->setText(QStringLiteral("--"));
        pct_used_label_->setText(QStringLiteral("--"));
        if (avail_label_ && balance_ > 0)
            avail_label_->setText(fmt_money(balance_));
        else if (avail_label_)
            avail_label_->setText(QStringLiteral("--"));
        submit_subtitle_->setText(tr("Enter a quantity to preview"));
    }
}

void CryptoOrderEntry::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void CryptoOrderEntry::retranslateUi() {
    if (title_label_) title_label_->setText(tr("ORDER ENTRY"));
    if (mode_label_)  mode_label_->setText(is_paper_ ? tr("PAPER") : tr("LIVE"));

    // Account card titles
    if (balance_title_) balance_title_->setText(tr("BALANCE"));
    if (mark_title_)    mark_title_->setText(tr("MARK"));
    if (avail_title_)   avail_title_->setText(tr("AVAIL"));

    // Side tabs
    if (buy_tab_)  buy_tab_->setText(tr("BUY"));
    if (sell_tab_) sell_tab_->setText(tr("SELL"));

    // Order type segmented control
    const QString type_labels[] = {tr("MARKET"), tr("LIMIT"), tr("STOP"), tr("STOP-LMT")};
    for (int i = 0; i < 4; ++i)
        if (type_btns_[i]) type_btns_[i]->setText(type_labels[i]);

    // Field / section titles
    if (qty_title_)      qty_title_->setText(tr("QUANTITY"));
    if (price_title_)    price_title_->setText(tr("LIMIT PRICE"));
    if (stop_title_)     stop_title_->setText(tr("STOP PRICE"));
    if (sl_title_)       sl_title_->setText(tr("STOP LOSS"));
    if (tp_title_)       tp_title_->setText(tr("TAKE PROFIT"));
    if (post_only_check_) {
        post_only_check_->setText(tr("POST ONLY"));
        post_only_check_->setToolTip(tr("Maker protection: the exchange should reject/cancel the order if it would fill immediately."));
    }
    if (sl_edit_)        sl_edit_->setPlaceholderText(tr("Trigger price"));
    if (tp_edit_)        tp_edit_->setPlaceholderText(tr("Trigger price"));
    if (leverage_title_) leverage_title_->setText(tr("LEVERAGE"));
    if (margin_title_)   margin_title_->setText(tr("MARGIN MODE"));

    // Margin mode combo items (preserve userData / selection)
    if (margin_mode_combo_) {
        margin_mode_combo_->setItemText(0, tr("Cross"));
        margin_mode_combo_->setItemText(1, tr("Isolated"));
    }

    // Order breakdown titles
    if (cost_title_)     cost_title_->setText(tr("ORDER VALUE"));
    if (fee_title_)      fee_title_->setText(tr("COST EST"));
    if (total_title_)    total_title_->setText(tr("TOTAL"));
    if (recv_title_)     recv_title_->setText(tr("YOU RECEIVE"));
    if (pct_used_title_) pct_used_title_->setText(tr("BREAKEVEN"));

    // Advanced toggle reflects current expanded state
    if (advanced_toggle_ && advanced_section_)
        advanced_toggle_->setText(advanced_section_->isVisible() ? tr("▴  Advanced  (SL / TP)")
                                                                 : tr("▾  Advanced  (SL / TP)"));

    // Submit button + tooltip + subtitle
    if (submit_btn_) {
        if (!submit_busy_)
            submit_btn_->setText(is_buy_side_ ? tr("BUY  %1").arg(current_symbol_)
                                              : tr("SELL  %1").arg(current_symbol_));
        submit_btn_->setToolTip(tr("Submit order  (⌘/Ctrl+Enter)"));
    }

    // Recompute the live cost preview so the subtitle / breakdown values pick
    // up the new language; covers both the populated and the placeholder state.
    update_cost_preview();
}

} // namespace openmarketterminal::screens::crypto
