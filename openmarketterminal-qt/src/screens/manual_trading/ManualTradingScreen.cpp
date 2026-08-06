#include "screens/manual_trading/ManualTradingScreen.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::screens {

namespace pred = openmarketterminal::services::prediction;
namespace kalshi_data = openmarketterminal::services::prediction::kalshi_ns;

namespace {

static const char* MF = "font-family:'Consolas',monospace;";

QString table_style() {
    return QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                   "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;"
                   "font-size:10px;font-weight:700;%4}"
                   "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QTableWidgetItem* cell(const QString& text, const QString& color = {}) {
    auto* it = new QTableWidgetItem(text);
    if (!color.isEmpty())
        it->setForeground(QColor(color));
    return it;
}

double outcome_price(const pred::PredictionMarket& market, int index) {
    return index >= 0 && index < market.outcomes.size() ? market.outcomes[index].price : 0.0;
}

QString money(double value) { return QStringLiteral("$%1").arg(value, 0, 'f', 2); }
QString signed_money(double value) { return QString::asprintf("%+.2f", value); }
QString probability(double value) {
    return QStringLiteral("%1%").arg(std::round(value * 100.0), 0, 'f', 0);
}

// The two accounts are backed by the same live BTC order book. Both crypto
// cadences the selector offers are Kalshi's KXBTC* series (15-minute:
// KXBTC15M, hourly: KXBTC…); filter whatever the shared adapter returns down
// to those so a foreign category payload (weather, another asset) landing on
// the shared events_ready/markets_ready signal can never repopulate the BTC
// selector.
bool is_btc_market(const pred::PredictionMarket& market) {
    const QString series = market.extras.value(QStringLiteral("series_ticker")).toString();
    if (series.startsWith(QStringLiteral("KXBTC")))
        return true;
    return market.key.market_id.startsWith(QStringLiteral("KXBTC"));
}

QString contract_label(const pred::PredictionMarket& market) {
    QString label = market.question.trimmed();
    if (label.isEmpty())
        label = market.extras.value(QStringLiteral("event_title")).toString().trimmed();
    if (label.isEmpty())
        label = market.key.market_id;
    return label;
}

QString close_suffix(const pred::PredictionMarket& market) {
    const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    if (!close.isValid())
        return {};
    return QStringLiteral("  · closes %1").arg(close.toLocalTime().toString(QStringLiteral("HH:mm")));
}

// True when a contract is worth showing in the selector: still open and not
// already past its close (with a small grace window).
bool selectable(const pred::PredictionMarket& market) {
    if (market.closed)
        return false;
    const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    if (close.isValid() && QDateTime::currentDateTimeUtc().secsTo(close.toUTC()) < -300)
        return false;
    return true;
}

QLabel* make_stat_value(const QString& initial = QStringLiteral("-")) {
    auto* val = new QLabel(initial);
    val->setStyleSheet(QString("color:%1;font-size:14px;font-weight:800;background:transparent;%2")
                           .arg(ui::colors::TEXT_PRIMARY(), MF));
    return val;
}

} // namespace

// manual_position_pnl() lives in ManualTradingPnl.cpp (pure, Qt-Core-only) so
// the teaching invariant can be unit-tested without linking the widget graph.

ManualTradingScreen::ManualTradingScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    wire_adapter();

    // Keep the marks and the BTC ladder current while this page is visible.
    // One list_events per tick (the current cadence only), so the shared
    // adapter never has two BTC fetches in flight at once.
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(15'000);
    connect(refresh_timer_, &QTimer::timeout, this, &ManualTradingScreen::refresh);
}

void ManualTradingScreen::build_ui() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto* title = new QLabel(tr("MANUAL TRADING (paper) — two accounts, one BTC order book"));
    title->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                             .arg(ui::colors::AMBER(), MF));
    root->addWidget(title);

    auto* subtitle = new QLabel(tr("KALSHI and COINBASE both read the SAME live Kalshi BTC feed (Coinbase has no "
                                   "API — same book). Separate paper ledgers, so YES on one and NO on the other "
                                   "do NOT net."));
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                .arg(ui::colors::TEXT_TERTIARY(), MF));
    root->addWidget(subtitle);

    // Selector row: cadence toggle · BTC contract combo · refresh · status.
    auto* sel_row = new QHBoxLayout;
    sel_row->setSpacing(10);

    cadence_bar_ = new QWidget(this);
    auto* cadence_layout = new QHBoxLayout(cadence_bar_);
    cadence_layout->setContentsMargins(0, 0, 0, 0);
    cadence_layout->setSpacing(4);
    const QVector<QPair<QString, QString>> cadences = {{QStringLiteral("15 MIN"), QStringLiteral("fifteen_min")},
                                                       {QStringLiteral("1 HOUR"), QStringLiteral("hourly")}};
    for (const auto& c : cadences) {
        auto* button = new QPushButton(c.first, cadence_bar_);
        button->setCheckable(true);
        button->setChecked(c.second == cadence_);
        button->setStyleSheet(QString(
            "QPushButton{background:%1;color:%2;border:1px solid %3;padding:3px 12px;font-size:10px;font-weight:700;%4}"
            "QPushButton:checked{background:%5;color:%6;border-color:%6;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::BG_BASE(), ui::colors::CYAN()));
        const QString cadence = c.second;
        connect(button, &QPushButton::clicked, this, [this, cadence] { set_cadence(cadence); });
        cadence_layout->addWidget(button);
    }
    sel_row->addWidget(cadence_bar_);

    auto* combo_label = new QLabel(tr("CONTRACT"));
    combo_label->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                   .arg(ui::colors::TEXT_SECONDARY(), MF));
    sel_row->addWidget(combo_label);

    market_combo_ = new QComboBox(this);
    market_combo_->setMinimumWidth(360);
    market_combo_->setToolTip(tr("Pick a Kalshi BTC contract. Both accounts price against this same market."));
    connect(market_combo_, &QComboBox::currentIndexChanged, this, [this](int) { on_market_selected(); });
    sel_row->addWidget(market_combo_, 1);

    refresh_button_ = new QPushButton(tr("REFRESH"), this);
    refresh_button_->setStyleSheet(QString(
        "QPushButton{background:%1;color:%2;border:1px solid %3;padding:3px 12px;font-size:10px;font-weight:700;%4}"
        "QPushButton:hover{color:%5;border-color:%5;}")
        .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::CYAN()));
    connect(refresh_button_, &QPushButton::clicked, this, &ManualTradingScreen::refresh);
    sel_row->addWidget(refresh_button_);

    root->addLayout(sel_row);

    status_label_ = new QLabel(tr("Loading BTC contracts…"));
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), MF));
    root->addWidget(status_label_);

    // Two account panels, side by side. Both read the same selected market.
    auto* panels_row = new QHBoxLayout;
    panels_row->setSpacing(12);
    panels_row->addWidget(build_account_panel(0, tr("KALSHI"), tr("Live Kalshi BTC order book")), 1);
    panels_row->addWidget(build_account_panel(1, tr("COINBASE (powered by Kalshi)"),
                                              tr("Same Kalshi BTC order book — Coinbase has no separate API")),
                          1);
    root->addLayout(panels_row, 1);

    build_scoreboard(root);
}

QWidget* ManualTradingScreen::build_account_panel(int idx, const QString& name, const QString& subtitle) {
    auto& p = panels_[idx];
    p.name = idx == 0 ? QStringLiteral("KALSHI") : QStringLiteral("COINBASE");

    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:4px;}")
                             .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    auto* col = new QVBoxLayout(frame);
    col->setContentsMargins(12, 10, 12, 10);
    col->setSpacing(8);

    auto* header = new QLabel(name);
    header->setStyleSheet(QString("color:%1;font-size:13px;font-weight:800;background:transparent;%2")
                              .arg(idx == 0 ? ui::colors::CYAN() : ui::colors::AMBER(), MF));
    col->addWidget(header);

    auto* sub = new QLabel(subtitle);
    sub->setWordWrap(true);
    sub->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;%2")
                           .arg(ui::colors::TEXT_TERTIARY(), MF));
    col->addWidget(sub);

    // HARD REQUIREMENT: a prominent, always-visible paper-only label on each
    // panel. This is not the fill-status line — it is the standing guarantee.
    auto* paper = new QLabel(tr("PAPER ONLY — no real order is ever submitted"));
    paper->setAlignment(Qt::AlignCenter);
    paper->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %1;border-radius:3px;padding:4px 8px;"
        "font-size:10px;font-weight:800;letter-spacing:1px;%3")
        .arg(ui::colors::AMBER(), ui::colors::BG_RAISED(), MF));
    col->addWidget(paper);

    // Live YES/NO price for the selected market (identical on both panels).
    auto* price_row = new QHBoxLayout;
    price_row->setSpacing(16);
    p.yes_price = new QLabel(QStringLiteral("YES —"));
    p.yes_price->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                                   .arg(ui::colors::POSITIVE(), MF));
    p.no_price = new QLabel(QStringLiteral("NO —"));
    p.no_price->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                                  .arg(ui::colors::CYAN(), MF));
    price_row->addWidget(p.yes_price);
    price_row->addWidget(p.no_price);
    price_row->addStretch();
    col->addLayout(price_row);

    // Order form.
    auto* form = new QHBoxLayout;
    form->setSpacing(8);
    p.side = new QComboBox(frame);
    p.side->addItems({QStringLiteral("YES"), QStringLiteral("NO")});
    p.side->setToolTip(tr("Outcome to buy on this account."));
    connect(p.side, &QComboBox::currentTextChanged, this, [this, idx](const QString&) {
        // Re-seed the price to the newly selected side's live mid so switching
        // YES/NO does not leave a stale price from the other outcome.
        pred::PredictionMarket m;
        if (!current_market(&m))
            return;
        const int oi = panels_[idx].side->currentText() == QStringLiteral("NO") ? 1 : 0;
        const double mid = outcome_price(m, oi);
        if (mid > 0.0 && panels_[idx].price)
            panels_[idx].price->setValue(mid);
    });
    form->addWidget(p.side);

    p.price = new QDoubleSpinBox(frame);
    p.price->setRange(0.01, 0.99);
    p.price->setSingleStep(0.01);
    p.price->setDecimals(2);
    p.price->setValue(0.50);
    p.price->setPrefix(QStringLiteral("$"));
    p.price->setToolTip(tr("Limit price per contract (probability, 1-99c)."));
    form->addWidget(p.price);

    p.qty = new QSpinBox(frame);
    p.qty->setRange(1, 100);
    p.qty->setValue(5);
    p.qty->setSuffix(tr(" ct"));
    p.qty->setToolTip(tr("Contracts (paper size)."));
    form->addWidget(p.qty);
    col->addLayout(form);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    p.preview = new QPushButton(tr("PREVIEW"), frame);
    connect(p.preview, &QPushButton::clicked, this, [this, idx] { preview_order(idx); });
    buttons->addWidget(p.preview);

    p.place = new QPushButton(tr("PLACE PAPER ORDER"), frame);
    p.place->setStyleSheet(QString("QPushButton{background:%1;color:%2;font-weight:800;padding:4px 10px;}")
                               .arg(ui::colors::BG_RAISED(), ui::colors::AMBER()));
    connect(p.place, &QPushButton::clicked, this, [this, idx] { place_order(idx); });
    buttons->addWidget(p.place);
    col->addLayout(buttons);

    p.confirm = new QLabel(tr("No paper fills yet on this account."));
    p.confirm->setWordWrap(true);
    p.confirm->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:10px;font-weight:700;%4")
        .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), MF));
    col->addWidget(p.confirm);

    auto* pos_hdr = new QLabel(tr("OPEN PAPER POSITIONS"));
    pos_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::CYAN(), MF));
    col->addWidget(pos_hdr);

    p.positions = new QTableWidget(0, 7, frame);
    p.positions->setHorizontalHeaderLabels(
        {tr("Time"), tr("Contract"), tr("Side"), tr("Qty"), tr("Entry"), tr("Fee"), tr("P&L")});
    p.positions->setStyleSheet(table_style());
    p.positions->verticalHeader()->setVisible(false);
    p.positions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    p.positions->setSelectionMode(QAbstractItemView::NoSelection);
    p.positions->horizontalHeader()->setStretchLastSection(true);
    p.positions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    col->addWidget(p.positions, 1);

    return frame;
}

void ManualTradingScreen::build_scoreboard(QVBoxLayout* into) {
    auto* hdr = new QLabel(tr("COMBINED SCOREBOARD — marked to current mid (unrealized)"));
    hdr->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                           .arg(ui::colors::AMBER(), MF));
    into->addWidget(hdr);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(4);

    const QStringList col_names = {tr("KALSHI"), tr("COINBASE"), tr("COMBINED")};
    const QStringList row_names = {tr("Staked"), tr("Fees"), tr("Net P&L"), tr("Open positions")};

    // Column headers.
    for (int c = 0; c < 3; ++c) {
        auto* h = new QLabel(col_names[c]);
        h->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                             .arg(c == 2 ? ui::colors::TEXT_PRIMARY() : ui::colors::TEXT_TERTIARY(), MF));
        grid->addWidget(h, 0, c + 1);
    }

    for (int r = 0; r < row_names.size(); ++r) {
        auto* rn = new QLabel(row_names[r]);
        rn->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                              .arg(ui::colors::TEXT_SECONDARY(), MF));
        grid->addWidget(rn, r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            auto* v = make_stat_value();
            grid->addWidget(v, r + 1, c + 1);
            switch (r) {
                case 0: sb_staked_[c] = v; break;
                case 1: sb_fees_[c] = v; break;
                case 2: sb_pnl_[c] = v; break;
                case 3: sb_positions_[c] = v; break;
                default: break;
            }
        }
    }
    grid->setColumnStretch(4, 1);
    into->addLayout(grid);

    auto* lesson = new QLabel(tr("Lesson: hold YES on one account and NO on the other for the same contract and "
                                 "size — the combined Net P&L settles to roughly −fees. Same order book, so the "
                                 "price moves cancel; only the fees remain."));
    lesson->setWordWrap(true);
    lesson->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:6px 10px;font-size:10px;%4")
        .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), MF));
    into->addWidget(lesson);

    update_scoreboard();
}

pred::PredictionExchangeAdapter* ManualTradingScreen::adapter() const {
    return pred::PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
}

void ManualTradingScreen::wire_adapter() {
    auto* a = adapter();
    if (!a) {
        if (status_label_)
            status_label_->setText(tr("Adapter offline — Kalshi BTC markets unavailable."));
        return;
    }
    // events_ready/markets_ready are shared with KalshiScreen; handle_markets()
    // filters every payload down to BTC and ignores anything else, so a
    // foreign fetch on the shared adapter can never disturb this selector.
    connect(a, &pred::PredictionExchangeAdapter::events_ready, this, &ManualTradingScreen::populate_events);
    connect(a, &pred::PredictionExchangeAdapter::markets_ready, this, &ManualTradingScreen::handle_markets);
}

void ManualTradingScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
    if (refresh_timer_)
        refresh_timer_->start();
}

void ManualTradingScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (refresh_timer_)
        refresh_timer_->stop();
}

void ManualTradingScreen::set_cadence(const QString& cadence) {
    if (cadence == cadence_)
        return;
    cadence_ = cadence;
    if (cadence_bar_) {
        const QHash<QString, QString> labels = {{QStringLiteral("fifteen_min"), QStringLiteral("15 MIN")},
                                                {QStringLiteral("hourly"), QStringLiteral("1 HOUR")}};
        for (auto* button : cadence_bar_->findChildren<QPushButton*>())
            button->setChecked(button->text() == labels.value(cadence_));
    }
    // A new cadence is a different set of series — clear so a stale hourly
    // contract cannot linger in a 15-minute list.
    markets_by_id_.clear();
    selected_market_id_.clear();
    refresh();
}

void ManualTradingScreen::refresh() {
    auto* a = adapter();
    if (!a) {
        if (status_label_)
            status_label_->setText(tr("Adapter offline — Kalshi BTC markets unavailable."));
        return;
    }
    fetch_pending_ = true;
    if (status_label_ && markets_by_id_.isEmpty())
        status_label_->setText(tr("Loading BTC contracts…"));
    // One list_events for the current cadence only (never two in flight).
    a->list_events(QStringLiteral("Crypto#BTC@%1").arg(cadence_), QStringLiteral("volume"), 200, 0);
}

void ManualTradingScreen::populate_events(const QVector<pred::PredictionEvent>& events) {
    QVector<pred::PredictionMarket> flattened;
    for (const auto& event : events) {
        for (auto market : event.markets) {
            if (market.question.trimmed().isEmpty())
                market.question = event.title;
            market.extras.insert(QStringLiteral("event_title"), event.title);
            flattened.push_back(market);
        }
    }
    handle_markets(flattened);
}

void ManualTradingScreen::handle_markets(const QVector<pred::PredictionMarket>& markets) {
    // Accept only if the payload actually carries BTC contracts. Foreign
    // fetches on the shared adapter (weather, other assets, empty replies) are
    // ignored so they can never wipe an already-loaded selector.
    QVector<pred::PredictionMarket> btc;
    for (const auto& m : markets) {
        if (is_btc_market(m) && selectable(m))
            btc.push_back(m);
    }
    if (btc.isEmpty())
        return;

    fetch_pending_ = false;
    for (const auto& m : btc)
        markets_by_id_.insert(m.key.market_id, m);
    rebuild_selector();
    // Fresh marks → refresh both ledgers and the scoreboard.
    refresh_positions_table(0);
    refresh_positions_table(1);
    update_scoreboard();
}

void ManualTradingScreen::rebuild_selector() {
    if (!market_combo_)
        return;

    QVector<pred::PredictionMarket> sorted;
    sorted.reserve(markets_by_id_.size());
    for (const auto& m : markets_by_id_)
        if (selectable(m))
            sorted.push_back(m);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.end_date_iso != b.end_date_iso)
            return a.end_date_iso < b.end_date_iso;  // soonest close first
        return a.key.market_id < b.key.market_id;
    });

    // If the set of contracts is unchanged (the common case on a 15s refresh),
    // do NOT clear/re-add the combo — that would flicker the dropdown and
    // re-fire selection. Just refresh the live prices/marks against the fresh
    // market data and leave the selector (and any typed order) alone.
    QStringList ids;
    ids.reserve(sorted.size());
    for (const auto& m : sorted)
        ids.push_back(m.key.market_id);
    if (!ids.isEmpty() && ids == last_selector_ids_) {
        update_prices();
        if (status_label_)
            status_label_->setText(tr("%1 BTC contracts · both accounts price against the selected one.")
                                       .arg(sorted.size()));
        return;
    }
    last_selector_ids_ = ids;

    // Preserve the operator's selection across a rebuild if it still exists.
    const QString keep = selected_market_id_;
    rebuilding_selector_ = true;
    market_combo_->clear();
    int keep_row = -1;
    for (int i = 0; i < sorted.size(); ++i) {
        const auto& m = sorted[i];
        market_combo_->addItem(contract_label(m) + close_suffix(m), m.key.market_id);
        if (m.key.market_id == keep)
            keep_row = i;
    }
    rebuilding_selector_ = false;

    if (sorted.isEmpty()) {
        selected_market_id_.clear();
        if (status_label_)
            status_label_->setText(tr("No open BTC contracts for this cadence right now."));
        update_prices();
        return;
    }

    const int row = keep_row >= 0 ? keep_row : 0;
    market_combo_->setCurrentIndex(row);
    // setCurrentIndex fires on_market_selected() when the index actually
    // changes; force the update for the case where it stayed at the same row.
    on_market_selected();
    if (status_label_)
        status_label_->setText(tr("%1 BTC contracts · both accounts price against the selected one.")
                                   .arg(sorted.size()));
}

void ManualTradingScreen::on_market_selected() {
    if (rebuilding_selector_ || !market_combo_)
        return;
    const QVariant data = market_combo_->currentData();
    if (data.isValid())
        selected_market_id_ = data.toString();
    update_prices();
    update_scoreboard();
}

bool ManualTradingScreen::current_market(pred::PredictionMarket* out) const {
    const auto it = markets_by_id_.constFind(selected_market_id_);
    if (it == markets_by_id_.constEnd())
        return false;
    if (out)
        *out = *it;
    return true;
}

void ManualTradingScreen::update_prices() {
    pred::PredictionMarket m;
    const bool have = current_market(&m);
    // Only (re)seed the price spins when the SELECTED MARKET changes — a live
    // 15s refresh must move the displayed YES/NO mids but must never overwrite
    // a limit price the user is in the middle of typing. Switching side is
    // handled explicitly by the side combo's own handler.
    const bool market_changed = have && selected_market_id_ != last_priced_market_id_;
    for (auto& p : panels_) {
        if (!p.yes_price || !p.no_price)
            continue;
        if (!have) {
            p.yes_price->setText(QStringLiteral("YES —"));
            p.no_price->setText(QStringLiteral("NO —"));
            continue;
        }
        p.yes_price->setText(tr("YES %1").arg(probability(outcome_price(m, 0))));
        p.no_price->setText(tr("NO %1").arg(probability(outcome_price(m, 1))));
        if (market_changed && p.side && p.price) {
            const int oi = p.side->currentText() == QStringLiteral("NO") ? 1 : 0;
            const double mid = outcome_price(m, oi);
            if (mid > 0.0)
                p.price->setValue(mid);
        }
    }
    if (market_changed)
        last_priced_market_id_ = selected_market_id_;
    else if (!have)
        last_priced_market_id_.clear();
}

double ManualTradingScreen::mark_price(const QString& market_id, const QString& side) const {
    const auto it = markets_by_id_.constFind(market_id);
    if (it == markets_by_id_.constEnd())
        return -1.0;  // unknown — caller marks flat against entry
    return outcome_price(*it, side == QStringLiteral("NO") ? 1 : 0);
}

void ManualTradingScreen::preview_order(int idx) {
    auto& p = panels_[idx];
    pred::PredictionMarket m;
    if (!current_market(&m)) {
        QMessageBox::information(this, tr("Manual Trading"), tr("Select a BTC contract first."));
        return;
    }
    const QString side = p.side->currentText();
    const double price = p.price->value();
    const int qty = p.qty->value();
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(m, price, qty);
    QMessageBox::information(
        this, tr("Paper order preview — %1").arg(p.name),
        tr("BUY %1 %2 contracts on:\n%3\n\nAccount: %4\nPrice %5\nNotional %6\nMax fee estimate %7\n"
           "Max loss %8\nPayout if correct %9\n\nThis review submits nothing. PLACE PAPER ORDER simulates a "
           "local fill only — no live or exchange order is ever submitted.")
            .arg(side).arg(qty).arg(contract_label(m), p.name, probability(price), money(price * qty), money(fee))
            .arg(money(price * qty + fee), money(qty)));
}

void ManualTradingScreen::place_order(int idx) {
    auto& p = panels_[idx];
    pred::PredictionMarket m;
    if (!current_market(&m)) {
        QMessageBox::information(this, tr("Manual Trading"), tr("Select a BTC contract first."));
        return;
    }
    const QString side = p.side->currentText();
    const double price = p.price->value();
    const int qty = p.qty->value();

    const auto answer = QMessageBox::question(
        this, tr("Confirm paper order — %1").arg(p.name),
        tr("BUY %1 %2 contracts of %3 at %4 on the %5 account:\n%6\n\nThis is a PAPER order. It will never be "
           "submitted to Kalshi or any exchange — it only records a local simulated fill.")
            .arg(side).arg(qty).arg(side, probability(price), p.name, contract_label(m)),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    // PAPER ONLY: no OrderRequest is built and nothing is handed to
    // PredictionExchangeAdapter::place_order(). The fill is recorded here,
    // in-memory, as a PaperPosition and nowhere else.
    PaperPosition pos;
    pos.account = p.name;
    pos.market_id = m.key.market_id;
    pos.market_label = contract_label(m);
    pos.side = side;
    pos.entry_price = price;
    pos.qty = qty;
    pos.fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(m, price, qty);
    pos.placed_at = QDateTime::currentDateTime();
    positions_.push_back(pos);

    if (p.confirm) {
        p.confirm->setText(tr("PAPER FILL — BUY %1 %2 @ %3 on %4 · %5")
                               .arg(qty).arg(side).arg(probability(price), pos.market_label,
                                                       pos.placed_at.toString(QStringLiteral("hh:mm:ss"))));
        p.confirm->setStyleSheet(QString(
            "color:%1;background:%2;border:1px solid %1;padding:4px 8px;font-size:10px;font-weight:700;%3")
            .arg(ui::colors::POSITIVE(), ui::colors::BG_RAISED(), MF));
    }

    refresh_positions_table(idx);
    update_scoreboard();
}

void ManualTradingScreen::refresh_positions_table(int idx) {
    auto& p = panels_[idx];
    if (!p.positions)
        return;
    p.positions->setRowCount(0);
    int row = 0;
    // Newest first.
    for (int i = positions_.size() - 1; i >= 0; --i) {
        const auto& pos = positions_[i];
        if (pos.account != p.name)
            continue;
        const double mark = mark_price(pos.market_id, pos.side);
        const double pnl = manual_position_pnl(pos.entry_price, mark < 0.0 ? pos.entry_price : mark, pos.qty, pos.fee);
        p.positions->insertRow(row);
        p.positions->setItem(row, 0, cell(pos.placed_at.toString(QStringLiteral("hh:mm:ss"))));
        p.positions->setItem(row, 1, cell(pos.market_label));
        p.positions->setItem(row, 2, cell(pos.side, pos.side == QStringLiteral("YES") ? ui::colors::POSITIVE()
                                                                                      : ui::colors::CYAN()));
        p.positions->setItem(row, 3, cell(QString::number(pos.qty)));
        p.positions->setItem(row, 4, cell(probability(pos.entry_price)));
        p.positions->setItem(row, 5, cell(money(pos.fee)));
        p.positions->setItem(row, 6, cell(signed_money(pnl),
                                          pnl >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
        ++row;
    }
}

void ManualTradingScreen::update_scoreboard() {
    double staked[3] = {0, 0, 0};
    double fees[3] = {0, 0, 0};
    double pnl[3] = {0, 0, 0};
    int counts[3] = {0, 0, 0};

    for (const auto& pos : positions_) {
        const int acc = pos.account == QStringLiteral("KALSHI") ? 0 : 1;
        const double mark = mark_price(pos.market_id, pos.side);
        const double position_pnl =
            manual_position_pnl(pos.entry_price, mark < 0.0 ? pos.entry_price : mark, pos.qty, pos.fee);
        const double stake = pos.entry_price * pos.qty;
        for (int c : {acc, 2}) {
            staked[c] += stake;
            fees[c] += pos.fee;
            pnl[c] += position_pnl;
            counts[c] += 1;
        }
    }

    for (int c = 0; c < 3; ++c) {
        if (sb_staked_[c])
            sb_staked_[c]->setText(money(staked[c]));
        if (sb_fees_[c])
            sb_fees_[c]->setText(money(fees[c]));
        if (sb_positions_[c])
            sb_positions_[c]->setText(QString::number(counts[c]));
        if (sb_pnl_[c]) {
            sb_pnl_[c]->setText(signed_money(pnl[c]));
            sb_pnl_[c]->setStyleSheet(QString("color:%1;font-size:14px;font-weight:800;background:transparent;%2")
                                          .arg(pnl[c] >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
        }
    }
}

} // namespace openmarketterminal::screens
