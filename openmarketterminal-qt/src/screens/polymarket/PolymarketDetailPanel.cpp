#include "screens/polymarket/PolymarketDetailPanel.h"

#include "screens/polymarket/PolymarketActivityFeed.h"
#include "screens/polymarket/PolymarketOrderBook.h"
#include "screens/polymarket/PolymarketPriceChart.h"
#include "services/edge_radar/BtcFiveMinuteEdgeModel.h"
#include "services/edge_radar/CryptoHourlyEdgeModel.h"
#include "services/edge_radar/EdgeRadarService.h"
#include "services/edge_radar/KalshiUniversalEdgeModel.h"
#include "storage/repositories/EdgeRadarRepository.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QTimeZone>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cmath>

namespace openmarketterminal::screens::polymarket {

using namespace openmarketterminal::ui;
namespace pmx = openmarketterminal::services::polymarket;
namespace edge = openmarketterminal::services::edge_radar;
namespace latency = openmarketterminal::services::crypto_latency;
using namespace openmarketterminal::services::prediction;

static const char* OUTCOME_COLORS[] = {"#00D66F", "#FF3B3B", "#FF8800", "#4F8EF7", "#A855F7"};

namespace {

QString edge_input_style() {
    return QString("QDoubleSpinBox, QTextEdit {"
                   "  background: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 2px; padding: 5px 8px; font-size: 11px;"
                   "}"
                   "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0px; border: none; }")
        .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::BORDER_MED());
}

QString edge_button_style(const QColor& accent, bool primary) {
    if (primary) {
        return QString("QPushButton { background: %1; color: #060606; border: none;"
                       "  border-radius: 2px; font-size: 11px; font-weight: 800; }"
                       "QPushButton:hover { background: %2; }")
            .arg(accent.name(), accent.lighter(115).name());
    }
    return QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 2px; font-size: 11px; font-weight: 800; }"
                   "QPushButton:hover { background: %4; color: %5; }")
        .arg(colors::BG_BASE(), accent.name(), accent.name(), colors::BG_HOVER(), colors::TEXT_PRIMARY());
}

QLabel* make_edge_caption(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(
        QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.7px;"
                " background: transparent;")
            .arg(colors::TEXT_SECONDARY()));
    return lbl;
}

QDoubleSpinBox* make_edge_spin(double value, const QString& suffix = QStringLiteral("%")) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(0.0, 100.0);
    spin->setDecimals(2);
    spin->setSingleStep(1.0);
    spin->setSuffix(suffix.isEmpty() ? QString{} : QStringLiteral(" ") + suffix);
    spin->setValue(value);
    spin->setStyleSheet(edge_input_style());
    return spin;
}

QString fmt_edge_pct(double v) {
    return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 2);
}

QString fmt_impulse_pct(double v) {
    const QString sign = v > 0.0 ? QStringLiteral("+") : QString{};
    return QStringLiteral("%1%2%").arg(sign).arg(v, 0, 'f', 3);
}

double parse_ticket_number(QString text, bool* ok) {
    text = text.trimmed();
    text.remove(QLatin1Char('$'));
    text.remove(QLatin1Char(','));
    text.remove(QLatin1Char('%'));
    text.remove(QStringLiteral("¢"));
    if (text.endsWith(QLatin1Char('c'), Qt::CaseInsensitive))
        text.chop(1);
    return text.trimmed().toDouble(ok);
}

double parse_ticket_price(QString text, bool* ok) {
    double price = parse_ticket_number(std::move(text), ok);
    if (!*ok) return 0.0;
    // Let humans enter either "0.59" or "59" / "59c" for a 59 cent contract.
    if (price > 1.0 && price <= 99.0)
        price /= 100.0;
    return price;
}

} // namespace

PolymarketDetailPanel::PolymarketDetailPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("polyDetailPanel");
    build_ui();
    feed_race_service_ = new latency::CryptoLatencyService(this);
    feed_race_timer_ = new QTimer(this);
    feed_race_timer_->setInterval(500);
    connect(feed_race_timer_, &QTimer::timeout, this, &PolymarketDetailPanel::refresh_feed_race);
    connect(feed_race_service_, &latency::CryptoLatencyService::snapshot_changed,
            this, &PolymarketDetailPanel::render_feed_race);
    connect(feed_race_service_, &latency::CryptoLatencyService::tick_received,
            this, [this](const latency::CryptoLatencyTick& tick) {
                impulse_model_.add_tick(tick);
                microstructure_model_.add_tick(tick);
                render_impulse(impulse_model_.signal(15));
            });
}

void PolymarketDetailPanel::build_ui() {
    auto* vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // ── Tab bar ───────────────────────────────────────────────────────────
    auto* tab_bar = new QWidget(this);
    tab_bar->setObjectName("polyDetailTabBar");
    tab_bar->setFixedHeight(34);
    tab_bar->setStyleSheet(
        QString("QWidget#polyDetailTabBar { background: %1; border-bottom: 1px solid %2; }")
            .arg(colors::BG_RAISED(), colors::BORDER_DIM()));

    auto* thl = new QHBoxLayout(tab_bar);
    thl->setContentsMargins(0, 0, 0, 0);
    thl->setSpacing(0);

    const QStringList tab_names = {
        tr("OVERVIEW"), tr("ORDER BOOK"), tr("CHART"), tr("TRADE"),
        tr("TRADES"), tr("EDGE"), tr("HOLDERS"), tr("COMMENTS"), tr("RELATED")
    };
    for (int i = 0; i < tab_names.size(); ++i) {
        auto* btn = new QPushButton(tab_names[i]);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("active", i == 0);
        connect(btn, &QPushButton::clicked, this, [this, i]() { set_active_tab(i); });
        thl->addWidget(btn);
        tab_btns_.append(btn);
    }
    thl->addStretch(1);
    vl->addWidget(tab_bar);
    apply_accent_to_tabs();

    // ── Stacked pages ─────────────────────────────────────────────────────
    stack_ = new QStackedWidget;
    stack_->addWidget(create_overview_page()); // 0

    orderbook_ = new PolymarketOrderBook;
    stack_->addWidget(orderbook_); // 1

    price_chart_ = new PolymarketPriceChart;
    connect(price_chart_, &PolymarketPriceChart::interval_changed,
            this, &PolymarketDetailPanel::interval_changed);
    connect(price_chart_, &PolymarketPriceChart::outcome_changed,
            this, &PolymarketDetailPanel::outcome_changed);
    stack_->addWidget(price_chart_); // 2

    stack_->addWidget(create_trade_page()); // 3

    activity_feed_ = new PolymarketActivityFeed;
    stack_->addWidget(activity_feed_); // 4

    stack_->addWidget(create_edge_page());     // 5
    stack_->addWidget(create_holders_page());  // 6
    stack_->addWidget(create_comments_page()); // 7
    stack_->addWidget(create_related_page());  // 8

    vl->addWidget(stack_, 1);
}

QWidget* PolymarketDetailPanel::create_overview_page() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        QString("QScrollArea { border: none; background: %1; }"
                "QScrollBar:vertical { background: %2; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %3; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::BG_SURFACE(), colors::BORDER_BRIGHT()));

    auto* page = new QWidget;
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(16, 14, 16, 16);
    vl->setSpacing(0);

    // ── Market question / title ───────────────────────────────────────────
    question_label_ = new QLabel(tr("Select a market to view details"));
    question_label_->setStyleSheet(
        QString("color: %1; font-size: 13px; font-weight: 700; background: transparent; "
                "line-height: 1.4;")
            .arg(colors::TEXT_PRIMARY()));
    question_label_->setWordWrap(true);
    question_label_->setMinimumHeight(36);
    vl->addWidget(question_label_);

    vl->addSpacing(10);

    // ── Status row (badge + outcome labels) ───────────────────────────────
    auto* status_row = new QWidget;
    status_row->setStyleSheet("background: transparent;");
    auto* srl = new QHBoxLayout(status_row);
    srl->setContentsMargins(0, 0, 0, 0);
    srl->setSpacing(6);

    status_label_ = new QLabel;
    srl->addWidget(status_label_);
    srl->addStretch(1);
    vl->addWidget(status_row);

    vl->addSpacing(12);

    // ── Stats: two rows of 3-4 cells each ────────────────────────────────
    // Row 1 of stats
    auto* stats_row1 = new QWidget;
    stats_row1->setStyleSheet(
        QString("background: %1; border: 1px solid %2; border-bottom: none;")
            .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* sr1l = new QHBoxLayout(stats_row1);
    sr1l->setContentsMargins(0, 0, 0, 0);
    sr1l->setSpacing(0);

    auto make_stat_in = [&](QWidget* parent_row, QHBoxLayout* row_layout,
                             const QString& lbl, QLabel*& val,
                             bool last = false, QWidget** box_out = nullptr) {
        auto* box = new QWidget(parent_row);
        box->setStyleSheet(
            last ? QString("background: transparent;")
                 : QString("background: transparent; border-right: 1px solid %1;")
                       .arg(colors::BORDER_DIM()));
        auto* bvl = new QVBoxLayout(box);
        bvl->setContentsMargins(12, 8, 12, 8);
        bvl->setSpacing(3);

        auto* lbl_w = new QLabel(lbl, box);
        lbl_w->setStyleSheet(
            QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.8px; "
                    "background: transparent;")
                .arg(colors::TEXT_SECONDARY()));
        // Cache the caption label so retranslateUi can re-apply its text.
        stat_caption_lbls_.append(lbl_w);

        val = new QLabel("—", box);
        val->setStyleSheet(
            QString("color: %1; font-size: 12px; font-weight: 700; background: transparent;")
                .arg(colors::TEXT_PRIMARY()));

        bvl->addWidget(lbl_w);
        bvl->addWidget(val);
        row_layout->addWidget(box, 1);
        if (box_out) *box_out = box;
    };

    make_stat_in(stats_row1, sr1l, tr("VOLUME"),    volume_label_);
    make_stat_in(stats_row1, sr1l, tr("LIQUIDITY"), liquidity_label_);
    make_stat_in(stats_row1, sr1l, tr("OPEN INT"),  oi_label_, false, &oi_box_);
    make_stat_in(stats_row1, sr1l, tr("END DATE"),  end_date_label_, true);
    vl->addWidget(stats_row1);

    // Row 2 of stats
    auto* stats_row2 = new QWidget;
    stats_row2->setStyleSheet(
        QString("background: %1; border: 1px solid %2;")
            .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* sr2l = new QHBoxLayout(stats_row2);
    sr2l->setContentsMargins(0, 0, 0, 0);
    sr2l->setSpacing(0);

    make_stat_in(stats_row2, sr2l, tr("MIDPOINT"),   midpoint_label_);
    make_stat_in(stats_row2, sr2l, tr("SPREAD"),     spread_label_);
    make_stat_in(stats_row2, sr2l, tr("LAST TRADE"), last_trade_label_, true);
    vl->addWidget(stats_row2);

    vl->addSpacing(14);

    // ── Outcome probability bars ──────────────────────────────────────────
    outcomes_header_ = new QLabel(tr("OUTCOMES"));
    auto* outcomes_header = outcomes_header_;
    outcomes_header->setStyleSheet(
        QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.8px; "
                "background: transparent;")
            .arg(colors::TEXT_SECONDARY()));
    vl->addWidget(outcomes_header);
    vl->addSpacing(6);

    outcome_container_ = new QWidget;
    outcome_container_->setStyleSheet("background: transparent;");
    auto* ocl = new QVBoxLayout(outcome_container_);
    ocl->setContentsMargins(0, 0, 0, 0);
    ocl->setSpacing(6);
    vl->addWidget(outcome_container_);

    vl->addSpacing(14);

    // ── Description ───────────────────────────────────────────────────────
    description_label_ = new QLabel;
    description_label_->setStyleSheet(
        QString("color: %1; font-size: 10px; line-height: 1.5; background: transparent; "
                "border-top: 1px solid %2; padding-top: 12px;")
            .arg(colors::TEXT_SECONDARY(), colors::BORDER_DIM()));
    description_label_->setWordWrap(true);
    vl->addWidget(description_label_);

    vl->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

QWidget* PolymarketDetailPanel::create_trade_page() {
    // Two-state stack: 0 = no-account placeholder, 1 = ticket form.
    ticket_stack_ = new QStackedWidget;

    // ── State 0: no account connected ────────────────────────────────────────
    auto* no_acct = new QWidget;
    no_acct->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* nal = new QVBoxLayout(no_acct);
    nal->setAlignment(Qt::AlignCenter);
    nal->setSpacing(8);
    no_acct_msg_lbl_ = new QLabel(tr("Connect an account\nto place orders"));
    auto* msg_lbl = no_acct_msg_lbl_;
    msg_lbl->setAlignment(Qt::AlignCenter);
    msg_lbl->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent;").arg(colors::TEXT_DIM()));
    nal->addWidget(msg_lbl);
    ticket_stack_->addWidget(no_acct); // index 0

    // ── State 1: ticket form ─────────────────────────────────────────────────
    auto* form = new QWidget;
    form->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* fl = new QVBoxLayout(form);
    fl->setContentsMargins(16, 14, 16, 14);
    fl->setSpacing(10);

    // Balance + position header
    auto* hdr = new QWidget;
    hdr->setStyleSheet(
        QString("background: %1; border: 1px solid %2; border-radius: 3px;")
            .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* hdrl = new QHBoxLayout(hdr);
    hdrl->setContentsMargins(10, 7, 10, 7);
    hdrl->setSpacing(0);

    auto* bal_col = new QVBoxLayout;
    bal_col->setSpacing(2);
    bal_caption_lbl_ = new QLabel(tr("AVAILABLE"));
    auto* bal_lbl = bal_caption_lbl_;
    bal_lbl->setStyleSheet(
        QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.8px; "
                "background: transparent;").arg(colors::TEXT_SECONDARY()));
    ticket_balance_lbl_ = new QLabel("—");
    ticket_balance_lbl_->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: 700; background: transparent;")
            .arg(colors::TEXT_PRIMARY()));
    bal_col->addWidget(bal_lbl);
    bal_col->addWidget(ticket_balance_lbl_);

    auto* pos_col = new QVBoxLayout;
    pos_col->setSpacing(2);
    pos_col->setAlignment(Qt::AlignRight);
    pos_caption_lbl_ = new QLabel(tr("POSITION"));
    auto* pos_lbl = pos_caption_lbl_;
    pos_lbl->setAlignment(Qt::AlignRight);
    pos_lbl->setStyleSheet(
        QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.8px; "
                "background: transparent;").arg(colors::TEXT_SECONDARY()));
    ticket_position_lbl_ = new QLabel("—");
    ticket_position_lbl_->setAlignment(Qt::AlignRight);
    ticket_position_lbl_->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: 700; background: transparent;")
            .arg(colors::TEXT_PRIMARY()));
    pos_col->addWidget(pos_lbl);
    pos_col->addWidget(ticket_position_lbl_);

    hdrl->addLayout(bal_col);
    hdrl->addStretch(1);
    hdrl->addLayout(pos_col);
    fl->addWidget(hdr);

    // Side selector: BUY / SELL
    auto* side_row = new QWidget;
    side_row->setStyleSheet("background: transparent;");
    auto* srl = new QHBoxLayout(side_row);
    srl->setContentsMargins(0, 0, 0, 0);
    srl->setSpacing(0);

    ticket_buy_btn_  = new QPushButton(tr("BUY"));
    ticket_sell_btn_ = new QPushButton(tr("SELL"));
    for (auto* b : {ticket_buy_btn_, ticket_sell_btn_}) {
        b->setFixedHeight(32);
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    connect(ticket_buy_btn_,  &QPushButton::clicked, this, [this]() {
        ticket_side_ = "BUY";
        refresh_ticket_side_style();
        update_ticket_estimate();
    });
    connect(ticket_sell_btn_, &QPushButton::clicked, this, [this]() {
        ticket_side_ = "SELL";
        refresh_ticket_side_style();
        update_ticket_estimate();
    });
    srl->addWidget(ticket_buy_btn_);
    srl->addWidget(ticket_sell_btn_);
    fl->addWidget(side_row);

    // Outcome selector
    auto make_label = [&](const QString& text) -> QLabel* {
        auto* l = new QLabel(text);
        l->setStyleSheet(
            QString("color: %1; font-size: 8px; font-weight: 700; letter-spacing: 0.8px; "
                    "background: transparent;").arg(colors::TEXT_SECONDARY()));
        // Cache so retranslateUi can re-apply (these are fixed field captions).
        trade_form_caption_lbls_.append(l);
        return l;
    };
    const QString input_ss =
        QString("QComboBox, QLineEdit {"
                "  background: %1; color: %2; border: 1px solid %3;"
                "  border-radius: 2px; padding: 4px 8px; font-size: 11px;"
                "}"
                "QComboBox::drop-down { border: none; }"
                "QComboBox QAbstractItemView {"
                "  background: %1; color: %2; border: 1px solid %3; selection-background-color: %4;"
                "}")
            .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::BORDER_MED(), colors::BG_HOVER());

    fl->addWidget(make_label(tr("OUTCOME")));
    ticket_outcome_cb_ = new QComboBox;
    ticket_outcome_cb_->setStyleSheet(input_ss);
    connect(ticket_outcome_cb_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
                prefill_ticket_price_from_outcome(true);
                update_ticket_estimate();
            });
    fl->addWidget(ticket_outcome_cb_);

    fl->addWidget(make_label(tr("DOLLAR AMOUNT")));
    ticket_cash_edit_ = new QLineEdit;
    ticket_cash_edit_->setPlaceholderText(tr("25.00"));
    ticket_cash_edit_->setStyleSheet(input_ss);
    connect(ticket_cash_edit_, &QLineEdit::textChanged, this, &PolymarketDetailPanel::update_ticket_estimate);
    fl->addWidget(ticket_cash_edit_);

    // Price + calculated contracts row
    auto* ps_row = new QWidget;
    ps_row->setStyleSheet("background: transparent;");
    auto* psl = new QHBoxLayout(ps_row);
    psl->setContentsMargins(0, 0, 0, 0);
    psl->setSpacing(8);

    auto* price_col = new QVBoxLayout;
    price_col->setSpacing(4);
    price_col->addWidget(make_label(tr("LIMIT PRICE")));
    ticket_price_edit_ = new QLineEdit;
    ticket_price_edit_->setPlaceholderText("0.50");
    ticket_price_edit_->setStyleSheet(input_ss);
    connect(ticket_price_edit_, &QLineEdit::textChanged, this, &PolymarketDetailPanel::update_ticket_estimate);
    price_col->addWidget(ticket_price_edit_);

    auto* size_col = new QVBoxLayout;
    size_col->setSpacing(4);
    size_col->addWidget(make_label(tr("CONTRACTS")));
    ticket_size_edit_ = new QLineEdit;
    ticket_size_edit_->setPlaceholderText(tr("auto"));
    ticket_size_edit_->setStyleSheet(input_ss);
    connect(ticket_size_edit_, &QLineEdit::textChanged, this, &PolymarketDetailPanel::update_ticket_estimate);
    size_col->addWidget(ticket_size_edit_);

    psl->addLayout(price_col);
    psl->addLayout(size_col);
    fl->addWidget(ps_row);

    // Order type
    fl->addWidget(make_label(tr("ORDER DURATION")));
    ticket_type_cb_ = new QComboBox;
    ticket_type_cb_->setStyleSheet(input_ss);
    configure_ticket_order_types();
    fl->addWidget(ticket_type_cb_);

    ticket_estimate_lbl_ = new QLabel(tr("Enter a dollar amount and limit price."));
    ticket_estimate_lbl_->setWordWrap(true);
    ticket_estimate_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_SECONDARY()));
    fl->addWidget(ticket_estimate_lbl_);

    // Submit button
    ticket_submit_btn_ = new QPushButton(tr("PLACE ORDER"));
    ticket_submit_btn_->setFixedHeight(34);
    ticket_submit_btn_->setCursor(Qt::PointingHandCursor);
    connect(ticket_submit_btn_, &QPushButton::clicked, this, &PolymarketDetailPanel::on_submit_clicked);
    fl->addWidget(ticket_submit_btn_);

    // Status label (feedback)
    ticket_status_lbl_ = new QLabel;
    ticket_status_lbl_->setAlignment(Qt::AlignCenter);
    ticket_status_lbl_->setWordWrap(true);
    ticket_status_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    fl->addWidget(ticket_status_lbl_);

    fl->addStretch(1);
    ticket_stack_->addWidget(form); // index 1

    refresh_ticket_side_style();
    return ticket_stack_;
}

void PolymarketDetailPanel::refresh_ticket_side_style() {
    if (!ticket_buy_btn_ || !ticket_sell_btn_ || !ticket_submit_btn_) return;
    const bool is_buy = (ticket_side_ == "BUY");
    const QString active_bg   = is_buy ? colors::POSITIVE() : colors::NEGATIVE();
    const QString inactive_bg = colors::BG_RAISED();
    const QString active_text   = "#0A0A0A";
    const QString inactive_text = colors::TEXT_DIM();

    ticket_buy_btn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: none; "
                "font-size: 11px; font-weight: 700; }"
                "QPushButton:hover { opacity: 0.9; }")
            .arg(is_buy ? active_bg : inactive_bg,
                 is_buy ? active_text : inactive_text));
    ticket_sell_btn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: none; "
                "font-size: 11px; font-weight: 700; }"
                "QPushButton:hover { opacity: 0.9; }")
            .arg(!is_buy ? active_bg : inactive_bg,
                 !is_buy ? active_text : inactive_text));
    ticket_submit_btn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: none; "
                "border-radius: 2px; font-size: 12px; font-weight: 700; }"
                "QPushButton:hover { background: %3; }"
                "QPushButton:disabled { background: %4; color: %5; }")
            .arg(active_bg, active_text,
                 QColor(active_bg).lighter(115).name(),
                 colors::BG_RAISED(), colors::TEXT_DIM()));
}

void PolymarketDetailPanel::prefill_ticket_price_from_outcome(bool force) {
    if (!ticket_price_edit_ || !ticket_outcome_cb_ || !has_last_market_)
        return;
    const int outcome_idx = ticket_outcome_cb_->currentIndex();
    if (outcome_idx < 0 || outcome_idx >= last_market_.outcomes.size())
        return;
    if (!force && !ticket_price_edit_->text().trimmed().isEmpty())
        return;

    const double price = qBound(0.01, last_market_.outcomes[outcome_idx].price, 0.99);
    const QSignalBlocker block(ticket_price_edit_);
    ticket_price_edit_->setText(QString::number(price, 'f', 2));
}

void PolymarketDetailPanel::configure_ticket_order_types() {
    if (!ticket_type_cb_)
        return;

    const QString exchange_id = has_last_market_ ? last_market_.key.exchange_id : QString{};
    const QString current_code = ticket_type_cb_->currentData().toString();
    const QSignalBlocker block(ticket_type_cb_);
    ticket_type_cb_->clear();

    if (exchange_id == QStringLiteral("kalshi")) {
        ticket_type_cb_->addItem(tr("Keep open until canceled"), QStringLiteral("limit"));
        ticket_type_cb_->setToolTip(tr("Kalshi limit order: stays open until it fills, expires, or you cancel it."));
        return;
    }

    // Polymarket CLOB order-type codes. Visible text is plain English; the
    // hidden data is the exact protocol value sent to the adapter.
    ticket_type_cb_->addItem(tr("Good till canceled"), QStringLiteral("GTC"));
    ticket_type_cb_->addItem(tr("All or nothing now"), QStringLiteral("FOK"));
    ticket_type_cb_->addItem(tr("Fill what can now"), QStringLiteral("FAK"));
    ticket_type_cb_->setToolTip(tr("Good till canceled: order stays open. "
                                   "All or nothing now: fill the whole order immediately or cancel. "
                                   "Fill what can now: fill any available contracts immediately and cancel the rest."));
    if (!current_code.isEmpty()) {
        const int idx = ticket_type_cb_->findData(current_code);
        if (idx >= 0) ticket_type_cb_->setCurrentIndex(idx);
    }
}

void PolymarketDetailPanel::update_ticket_estimate() {
    if (!ticket_estimate_lbl_ || !ticket_price_edit_)
        return;

    bool price_ok = false;
    const double price = parse_ticket_price(ticket_price_edit_->text(), &price_ok);
    if (!price_ok || price <= 0.0 || price >= 1.0) {
        ticket_estimate_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_SECONDARY()));
        ticket_estimate_lbl_->setText(tr("Enter a limit price from 1c to 99c."));
        return;
    }

    bool cash_ok = false;
    const double cash = ticket_cash_edit_ ? parse_ticket_number(ticket_cash_edit_->text(), &cash_ok) : 0.0;
    bool size_ok = false;
    double contracts = ticket_size_edit_ ? parse_ticket_number(ticket_size_edit_->text(), &size_ok) : 0.0;

    if (cash_ok && cash > 0.0) {
        contracts = std::floor(cash / price);
        const QSignalBlocker size_block(ticket_size_edit_);
        ticket_size_edit_->setText(contracts > 0.0 ? QString::number(contracts, 'f', 0) : QString{});
    }

    if (contracts <= 0.0) {
        ticket_estimate_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_SECONDARY()));
        ticket_estimate_lbl_->setText(tr("Enter dollars to calculate contracts."));
        return;
    }

    const double notional = contracts * price;
    const QString action_word = (ticket_side_ == QStringLiteral("SELL")) ? tr("proceeds") : tr("cost");
    ticket_estimate_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_PRIMARY()));
    ticket_estimate_lbl_->setText(
        tr("%1 contracts at %2c = est. %3 $%4. Whole contracts only.")
            .arg(contracts, 0, 'f', 0)
            .arg(price * 100.0, 0, 'f', 0)
            .arg(action_word)
            .arg(notional, 0, 'f', 2));
}

void PolymarketDetailPanel::on_submit_clicked() {
    if (!has_last_market_) return;
    bool price_ok = false, size_ok = false, cash_ok = false;
    const double price = parse_ticket_price(ticket_price_edit_->text(), &price_ok);
    const double cash = ticket_cash_edit_ ? parse_ticket_number(ticket_cash_edit_->text(), &cash_ok) : 0.0;
    double size  = ticket_size_edit_ ? parse_ticket_number(ticket_size_edit_->text(), &size_ok) : 0.0;
    if (cash_ok && cash > 0.0) {
        size = std::floor(cash / price);
        size_ok = size > 0.0;
    }
    if (!price_ok || price <= 0.0 || price >= 1.0) {
        ticket_status_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::NEGATIVE()));
        ticket_status_lbl_->setText(tr("Invalid price — enter 1c to 99c"));
        return;
    }
    if (!size_ok || size <= 0.0) {
        ticket_status_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::NEGATIVE()));
        ticket_status_lbl_->setText(tr("Enter enough dollars for at least 1 contract"));
        return;
    }

    const int outcome_idx = ticket_outcome_cb_->currentIndex();
    const QString asset_id = (outcome_idx >= 0 && outcome_idx < last_market_.outcomes.size())
                                 ? last_market_.outcomes[outcome_idx].asset_id
                                 : QString{};

    OrderRequest req;
    req.key        = last_market_.key;
    req.asset_id   = asset_id;
    req.side       = ticket_side_;
    req.order_type = ticket_type_cb_->currentData().toString();
    if (req.order_type.isEmpty())
        req.order_type = ticket_type_cb_->currentText();
    req.price      = price;
    req.size       = std::floor(size);

    ticket_status_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    ticket_status_lbl_->setText(tr("Submitting…"));
    ticket_submit_btn_->setEnabled(false);
    emit place_order(req);
}

void PolymarketDetailPanel::set_balance(const AccountBalance& balance) {
    if (!ticket_balance_lbl_) return;
    ticket_balance_lbl_->setText(
        QString("%1 %2").arg(balance.available, 0, 'f', 2).arg(balance.currency));
}

void PolymarketDetailPanel::set_positions(const QVector<PredictionPosition>& positions) {
    if (!ticket_position_lbl_ || !has_last_market_) return;
    // Find the position matching the currently-displayed market.
    double total_size = 0.0;
    for (const auto& p : positions) {
        if (p.market_id == last_market_.key.market_id)
            total_size += p.size;
    }
    ticket_position_lbl_->setText(total_size > 0.0
                                      ? tr("%1 shares").arg(total_size, 0, 'f', 2)
                                      : tr("No position"));
}

void PolymarketDetailPanel::on_order_result(const OrderResult& result) {
    if (!ticket_status_lbl_ || !ticket_submit_btn_) return;
    ticket_submit_btn_->setEnabled(true);
    if (result.ok) {
        ticket_status_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::POSITIVE()));
        ticket_status_lbl_->setText(
            tr("Order placed ✓  ID: %1").arg(result.order_id.left(12)));
        ticket_price_edit_->clear();
        if (ticket_cash_edit_) ticket_cash_edit_->clear();
        ticket_size_edit_->clear();
        if (ticket_estimate_lbl_) ticket_estimate_lbl_->setText(tr("Enter a dollar amount and limit price."));
    } else {
        ticket_status_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::NEGATIVE()));
        ticket_status_lbl_->setText(
            result.error_message.isEmpty() ? result.error_code : result.error_message);
    }
}

void PolymarketDetailPanel::set_trading_enabled(bool enabled) {
    if (!ticket_stack_) return;
    ticket_stack_->setCurrentIndex(enabled ? 1 : 0);
}

void PolymarketDetailPanel::update_edge_prefill_from_market() {
    if (!edge_market_lbl_ || !edge_market_prob_ || !edge_model_prob_ || !has_last_market_)
        return;

    const QString previous_market_id = edge_market_lbl_->property("market_id").toString();
    const bool new_market = previous_market_id != last_market_.key.market_id;
    double probability = 0.0;
    if (!last_market_.outcomes.isEmpty())
        probability = qBound(0.0, last_market_.outcomes.first().price, 1.0);

    edge_market_lbl_->setProperty("market_id", last_market_.key.market_id);
    edge_market_lbl_->setText(last_market_.question.isEmpty()
                                  ? last_market_.key.market_id
                                  : last_market_.question);

    const QSignalBlocker market_block(edge_market_prob_);
    edge_market_prob_->setValue(probability * 100.0);

    if (new_market) {
        const QSignalBlocker model_block(edge_model_prob_);
        edge_model_prob_->setValue(probability * 100.0);
        edge_model_prob_->setProperty("auto_market_id", QString{});
        if (edge_status_lbl_) edge_status_lbl_->clear();
    }

    apply_crypto_hourly_anchor();
    evaluate_edge_page();
}

void PolymarketDetailPanel::apply_crypto_hourly_anchor() {
    if (!has_last_market_ || !edge_anchor_lbl_ || !edge_model_prob_)
        return;

    edge::CryptoHourlySignal signal =
        edge::CryptoHourlyEdgeModel::score_market(last_market_, edge_context_markets_);
    bool used_live_impulse = false;
    const QString crypto_symbol = edge::CryptoHourlyEdgeModel::extract_symbol(
        last_market_.question + QStringLiteral(" ") + last_market_.key.market_id);
    const edge::CryptoImpulseSignal impulse = impulse_model_.signal(15);
    const double live_btc_anchor = edge::CryptoImpulseModel::anchor_probability(impulse);
    if (edge::BtcFiveMinuteEdgeModel::is_btc_five_minute_market(last_market_)) {
        edge::BtcFiveMinuteOptions options;
        options.spread_cost = edge_spread_cost_ ? edge_spread_cost_->value() / 100.0 : options.spread_cost;
        options.fee_cost = edge_fee_cost_ ? edge_fee_cost_->value() / 100.0 : options.fee_cost;
        options.minimum_liquidity_score = edge_liquidity_ ? edge_liquidity_->value() / 100.0
                                                          : options.minimum_liquidity_score;
        const edge::BtcFiveMinuteSignal btc5m =
            edge::BtcFiveMinuteEdgeModel::score_market(last_market_, impulse, options);
        const QString next_auto_id = QStringLiteral("btc5m:%1:%2:%3")
                                         .arg(last_market_.key.market_id)
                                         .arg(qRound(btc5m.model_probability * 10000.0))
                                         .arg(qRound(btc5m.move_usd * 10.0));
        if (edge_model_prob_ && edge_model_prob_->property("auto_market_id").toString() != next_auto_id) {
            const QSignalBlocker model_block(edge_model_prob_);
            edge_model_prob_->setValue(btc5m.model_probability * 100.0);
            edge_model_prob_->setProperty("auto_market_id", next_auto_id);
            edge_model_prob_->setProperty("auto_source", QStringLiteral("btc5m-live"));
        }
        if (edge_spread_cost_ && edge_spread_cost_->value() <= 0.0) {
            const QSignalBlocker spread_block(edge_spread_cost_);
            edge_spread_cost_->setValue(options.spread_cost * 100.0);
        }
        if (edge_liquidity_) {
            const QSignalBlocker liq_block(edge_liquidity_);
            edge_liquidity_->setValue(btc5m.liquidity_score * 100.0);
        }
        if (edge_confidence_) {
            const QSignalBlocker conf_block(edge_confidence_);
            edge_confidence_->setValue(btc5m.confidence * 100.0);
        }
        if (edge_thesis_ && edge_thesis_->toPlainText().trimmed().isEmpty())
            edge_thesis_->setPlainText(btc5m.rationale);
        if (edge_risk_notes_ && edge_risk_notes_->toPlainText().trimmed().isEmpty())
            edge_risk_notes_->setPlainText(btc5m.risk_notes);

        const QColor color = btc5m.passes_gate
                                 ? QColor(colors::POSITIVE())
                                 : (btc5m.recommendation == QStringLiteral("watch")
                                        ? QColor(colors::AMBER())
                                        : QColor(colors::TEXT_SECONDARY()));
        edge_anchor_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: 700; background: transparent;")
                .arg(color.name()));
        edge_anchor_lbl_->setText(tr("BTC 5m live: %1").arg(btc5m.rationale));
        if (edge_gate_lbl_) {
            edge_gate_lbl_->setStyleSheet(
                QString("color: %1; font-size: 12px; font-weight: 800; background: transparent;")
                    .arg(color.name()));
            edge_gate_lbl_->setText(btc5m.passes_gate
                                        ? (btc5m.is_strong ? tr("PASS STRONG") : tr("PASS"))
                                        : tr("REJECT"));
        }
        return;
    }
    if (!crypto_symbol.isEmpty() && impulse.gate == QStringLiteral("pass") && live_btc_anchor > 0.0) {
        edge::CryptoHourlyOptions options;
        options.spread_cost = edge_spread_cost_ ? edge_spread_cost_->value() / 100.0 : options.spread_cost;
        options.fee_cost = edge_fee_cost_ ? edge_fee_cost_->value() / 100.0 : options.fee_cost;
        options.minimum_liquidity_score = edge_liquidity_ ? edge_liquidity_->value() / 100.0
                                                          : options.minimum_liquidity_score;
        signal = edge::CryptoHourlyEdgeModel::score_symbol(
            crypto_symbol,
            edge::CryptoHourlyEdgeModel::yes_probability(last_market_),
            live_btc_anchor,
            options,
            edge::CryptoHourlyEdgeModel::infer_direction(last_market_.question),
            last_market_.liquidity,
            last_market_.question,
            last_market_.key.market_id);
        used_live_impulse = signal.is_valid;
    }

    if (!signal.is_valid || signal.btc_anchor_probability <= 0.0) {
        if (last_market_.key.exchange_id == QStringLiteral("kalshi")) {
            const edge::KalshiUniversalSignal universal =
                edge::KalshiUniversalEdgeModel::score_market(last_market_);
            edge_anchor_lbl_->setStyleSheet(
                QString("color: %1; font-size: 10px; font-weight: 700; background: transparent;")
                    .arg(colors::TEXT_SECONDARY()));
            edge_anchor_lbl_->setText(
                tr("%1 research: %2")
                    .arg(universal.family.toUpper())
                    .arg(universal.research_drivers.join(QStringLiteral(", "))));
            if (edge_gate_lbl_) {
                edge_gate_lbl_->setStyleSheet(
                    QString("color: %1; font-size: 12px; font-weight: 800; background: transparent;")
                        .arg(colors::TEXT_SECONDARY()));
                edge_gate_lbl_->setText(universal.gate == QStringLiteral("pass") ? tr("PASS") : tr("RESEARCH"));
            }
            if (edge_confidence_ && edge_confidence_->value() <= 0.0) {
                const QSignalBlocker conf_block(edge_confidence_);
                edge_confidence_->setValue(universal.confidence * 100.0);
            }
            if (edge_liquidity_) {
                const QSignalBlocker liq_block(edge_liquidity_);
                edge_liquidity_->setValue(universal.liquidity_score * 100.0);
            }
            if (edge_spread_cost_ && edge_spread_cost_->value() <= 0.0) {
                const QSignalBlocker spread_block(edge_spread_cost_);
                edge_spread_cost_->setValue(universal.spread_cost * 100.0);
            }
            if (edge_fee_cost_ && edge_fee_cost_->value() <= 0.0) {
                const QSignalBlocker fee_block(edge_fee_cost_);
                edge_fee_cost_->setValue(universal.fee_cost * 100.0);
            }
            if (edge_thesis_ && edge_thesis_->toPlainText().trimmed().isEmpty())
                edge_thesis_->setPlainText(universal.rationale);
            if (edge_risk_notes_ && edge_risk_notes_->toPlainText().trimmed().isEmpty())
                edge_risk_notes_->setPlainText(universal.risk_notes);
            if (edge_model_prob_)
                edge_model_prob_->setProperty("auto_source", QStringLiteral("kalshi-universal"));
            return;
        }
        edge_anchor_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
        edge_anchor_lbl_->setText(tr("BTC anchor: load Crypto + 1 Hour markets to auto-score hourly contracts"));
        if (edge_gate_lbl_)
            edge_gate_lbl_->setText(tr("NO ANCHOR"));
        return;
    }

    const QString auto_id = edge_model_prob_->property("auto_market_id").toString();
    const QString auto_source = used_live_impulse ? QStringLiteral("live-impulse") : QStringLiteral("market-context");
    const QString next_auto_id = used_live_impulse
                                     ? QStringLiteral("%1:%2").arg(last_market_.key.market_id)
                                           .arg(qRound(live_btc_anchor * 10000.0))
                                     : last_market_.key.market_id;
    if (auto_id != next_auto_id) {
        const QSignalBlocker model_block(edge_model_prob_);
        edge_model_prob_->setValue(signal.model_probability * 100.0);
        edge_model_prob_->setProperty("auto_market_id", next_auto_id);
        edge_model_prob_->setProperty("auto_source", auto_source);

        if (edge_fee_cost_ && edge_fee_cost_->value() <= 0.0) {
            const QSignalBlocker fee_block(edge_fee_cost_);
            edge_fee_cost_->setValue(1.75);
        }
        if (edge_spread_cost_ && edge_spread_cost_->value() <= 0.0) {
            const QSignalBlocker spread_block(edge_spread_cost_);
            edge_spread_cost_->setValue(2.0);
        }
        if (edge_liquidity_) {
            const QSignalBlocker liq_block(edge_liquidity_);
            edge_liquidity_->setValue(signal.liquidity_score * 100.0);
        }
        if (edge_confidence_) {
            const QSignalBlocker conf_block(edge_confidence_);
            edge_confidence_->setValue(signal.confidence * 100.0);
        }
        if (edge_thesis_ && edge_thesis_->toPlainText().trimmed().isEmpty())
            edge_thesis_->setPlainText(signal.rationale);
        if (edge_risk_notes_ && edge_risk_notes_->toPlainText().trimmed().isEmpty())
            edge_risk_notes_->setPlainText(signal.risk_notes);
    }

    const QColor color = signal.passes_gate
                             ? QColor(colors::POSITIVE())
                             : (signal.recommendation == QStringLiteral("watch")
                                    ? QColor(colors::AMBER())
                                    : QColor(colors::TEXT_SECONDARY()));
    edge_anchor_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 700; background: transparent;")
            .arg(color.name()));
    edge_anchor_lbl_->setText(used_live_impulse
                                  ? tr("LIVE BTC impulse: %1").arg(signal.rationale)
                                  : signal.rationale);
    if (edge_gate_lbl_) {
        edge_gate_lbl_->setStyleSheet(
            QString("color: %1; font-size: 12px; font-weight: 800; background: transparent;")
                .arg(color.name()));
        edge_gate_lbl_->setText(signal.passes_gate
                                    ? (signal.is_strong ? tr("PASS STRONG") : tr("PASS"))
                                    : tr("REJECT"));
    }
    if (edge_risk_notes_ && signal.gate == QStringLiteral("reject") &&
        edge_risk_notes_->toPlainText().trimmed().isEmpty()) {
        edge_risk_notes_->setPlainText(signal.risk_notes);
    }
}

void PolymarketDetailPanel::evaluate_edge_page() {
    if (!edge_side_lbl_ || !edge_market_prob_ || !edge_model_prob_)
        return;

    edge::EdgeInputs inputs;
    inputs.market_probability = edge_market_prob_->value() / 100.0;
    inputs.model_probability = edge_model_prob_->value() / 100.0;
    inputs.spread_cost = edge_spread_cost_ ? edge_spread_cost_->value() / 100.0 : 0.0;
    inputs.fee_cost = edge_fee_cost_ ? edge_fee_cost_->value() / 100.0 : 0.0;
    inputs.liquidity_score = edge_liquidity_ ? edge_liquidity_->value() / 100.0 : 0.0;
    inputs.confidence = edge_confidence_ ? edge_confidence_->value() / 100.0 : 0.0;

    const edge::EdgeScore score = edge::EdgeRadarService::evaluate(inputs);
    const QColor reco_color = score.recommendation == QStringLiteral("candidate")
                                  ? QColor(colors::POSITIVE())
                                  : (score.recommendation == QStringLiteral("watch")
                                         ? QColor(colors::AMBER())
                                         : QColor(colors::NEGATIVE()));

    edge_side_lbl_->setText(score.side.toUpper());
    edge_raw_lbl_->setText(fmt_edge_pct(score.raw_edge));
    edge_net_lbl_->setText(fmt_edge_pct(score.edge_after_cost));
    edge_reco_lbl_->setText(score.recommendation.toUpper());
    edge_reco_lbl_->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: 800; background: transparent;")
            .arg(reco_color.name()));
    edge_risk_lbl_->setText(score.risk_notes);
}

void PolymarketDetailPanel::set_edge_market_context(const QVector<PredictionMarket>& markets) {
    edge_context_markets_ = markets;
    apply_crypto_hourly_anchor();
    evaluate_edge_page();
}

void PolymarketDetailPanel::save_edge_idea() {
    if (!has_last_market_) {
        if (edge_status_lbl_) {
            edge_status_lbl_->setStyleSheet(
                QString("color: %1; font-size: 10px; background: transparent;").arg(colors::NEGATIVE()));
            edge_status_lbl_->setText(tr("Select a market before saving an edge idea"));
        }
        return;
    }

    evaluate_edge_page();

    edge::EdgeInputs inputs;
    inputs.market_probability = edge_market_prob_ ? edge_market_prob_->value() / 100.0 : 0.0;
    inputs.model_probability = edge_model_prob_ ? edge_model_prob_->value() / 100.0 : 0.0;
    inputs.spread_cost = edge_spread_cost_ ? edge_spread_cost_->value() / 100.0 : 0.0;
    inputs.fee_cost = edge_fee_cost_ ? edge_fee_cost_->value() / 100.0 : 0.0;
    inputs.liquidity_score = edge_liquidity_ ? edge_liquidity_->value() / 100.0 : 0.0;
    inputs.confidence = edge_confidence_ ? edge_confidence_->value() / 100.0 : 0.0;
    const edge::EdgeScore score = edge::EdgeRadarService::evaluate(inputs);
    edge::CryptoHourlySignal crypto_signal =
        edge::CryptoHourlyEdgeModel::score_market(last_market_, edge_context_markets_);
    const QString auto_source = edge_model_prob_ ? edge_model_prob_->property("auto_source").toString() : QString{};
    const QString crypto_symbol = edge::CryptoHourlyEdgeModel::extract_symbol(
        last_market_.question + QStringLiteral(" ") + last_market_.key.market_id);
    if (auto_source == QStringLiteral("live-impulse") && !crypto_symbol.isEmpty()) {
        edge::CryptoHourlyOptions options;
        options.spread_cost = inputs.spread_cost;
        options.fee_cost = inputs.fee_cost;
        options.minimum_liquidity_score = inputs.liquidity_score;
        crypto_signal = edge::CryptoHourlyEdgeModel::score_symbol(
            crypto_symbol,
            edge::CryptoHourlyEdgeModel::yes_probability(last_market_),
            edge::CryptoImpulseModel::anchor_probability(impulse_model_.signal(15)),
            options,
            edge::CryptoHourlyEdgeModel::infer_direction(last_market_.question),
            last_market_.liquidity,
            last_market_.question,
            last_market_.key.market_id);
    }
    edge::BtcFiveMinuteSignal btc5m_signal;
    if (auto_source == QStringLiteral("btc5m-live")) {
        edge::BtcFiveMinuteOptions options;
        options.spread_cost = inputs.spread_cost;
        options.fee_cost = inputs.fee_cost;
        options.minimum_liquidity_score = inputs.liquidity_score;
        btc5m_signal = edge::BtcFiveMinuteEdgeModel::score_market(
            last_market_, impulse_model_.signal(15), options);
    }
    const bool use_crypto_signal = crypto_signal.is_valid && edge_model_prob_ &&
                                   !edge_model_prob_->property("auto_market_id").toString().isEmpty();
    const bool use_btc5m_signal = btc5m_signal.is_valid && auto_source == QStringLiteral("btc5m-live");

    EdgeRadarIdea idea;
    idea.asset_class = QStringLiteral("prediction");
    idea.venue = last_market_.key.exchange_id.isEmpty() ? QStringLiteral("prediction") : last_market_.key.exchange_id;
    idea.symbol = use_btc5m_signal ? QStringLiteral("BTC")
                                   : (crypto_symbol.isEmpty() ? last_market_.key.market_id : crypto_symbol);
    idea.market_id = last_market_.key.market_id;
    idea.question = last_market_.question;
    idea.side = use_btc5m_signal ? btc5m_signal.side : score.side;
    idea.market_probability = score.market_probability;
    idea.model_probability = score.model_probability;
    idea.spread_cost = inputs.spread_cost;
    idea.fee_cost = inputs.fee_cost;
    idea.liquidity_score = inputs.liquidity_score;
    idea.confidence = inputs.confidence;
    idea.raw_edge = use_btc5m_signal ? btc5m_signal.raw_edge
                                     : (use_crypto_signal ? crypto_signal.raw_edge : score.raw_edge);
    idea.edge_after_cost = use_btc5m_signal ? btc5m_signal.gate_edge
                                            : (use_crypto_signal ? crypto_signal.gate_edge
                                                                 : score.edge_after_cost);
    idea.recommendation = use_btc5m_signal ? btc5m_signal.recommendation
                                           : (use_crypto_signal ? crypto_signal.recommendation
                                                                : score.recommendation);
    idea.thesis = edge_thesis_ ? edge_thesis_->toPlainText().trimmed() : QString{};
    const QString manual_risk = edge_risk_notes_ ? edge_risk_notes_->toPlainText().trimmed() : QString{};
    idea.risk_notes = manual_risk.isEmpty()
                          ? (use_btc5m_signal ? btc5m_signal.risk_notes
                                              : (use_crypto_signal ? crypto_signal.risk_notes : score.risk_notes))
                          : manual_risk;
    idea.status = QStringLiteral("watching");
    idea.tags = use_btc5m_signal
                    ? QStringLiteral("embedded,prediction,btc5m,polymarket,live-impulse")
                    : (crypto_symbol.isEmpty()
                    ? QStringLiteral("embedded,prediction")
                    : (auto_source == QStringLiteral("live-impulse")
                           ? QStringLiteral("embedded,prediction,crypto-hourly,btc-live-impulse")
                           : QStringLiteral("embedded,prediction,crypto-hourly,btc-anchor")));

    auto created = EdgeRadarRepository::instance().create(idea);
    if (edge_status_lbl_) {
        if (created.is_err()) {
            edge_status_lbl_->setStyleSheet(
                QString("color: %1; font-size: 10px; background: transparent;").arg(colors::NEGATIVE()));
            edge_status_lbl_->setText(tr("Save failed: %1").arg(QString::fromStdString(created.error())));
        } else {
            edge_status_lbl_->setStyleSheet(
                QString("color: %1; font-size: 10px; background: transparent;").arg(colors::POSITIVE()));
            edge_status_lbl_->setText(tr("Saved to Edge Radar: %1").arg(created.value().id.left(8)));
        }
    }
}

void PolymarketDetailPanel::start_feed_race() {
    if (!feed_race_service_)
        return;

    impulse_model_.clear();
    microstructure_model_.clear();
    if (impulse_move_lbl_)
        impulse_move_lbl_->setText(tr("warming up"));
    if (impulse_velocity_lbl_)
        impulse_velocity_lbl_->setText(tr("velocity -"));
    if (impulse_call_lbl_)
        impulse_call_lbl_->setText(tr("no trade"));

    const QString symbol = latency::CryptoLatencyService::normalize_symbol(QStringLiteral("BTC-USD"));
    if (feed_race_symbol_lbl_)
        feed_race_symbol_lbl_->setText(tr("%1 anchor public exchange WebSockets").arg(symbol));
    if (feed_race_fresh_lbl_) {
        feed_race_fresh_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
        feed_race_fresh_lbl_->setText(tr("connecting"));
    }
    if (micro_call_lbl_)
        micro_call_lbl_->setText(tr("warming up"));
    if (micro_pressure_lbl_)
        micro_pressure_lbl_->setText(tr("tape - book -"));
    if (micro_divergence_lbl_)
        micro_divergence_lbl_->setText(tr("divergence -"));
    feed_race_service_->start(symbol);
    if (feed_race_timer_)
        feed_race_timer_->start();
    refresh_feed_race();
}

void PolymarketDetailPanel::stop_feed_race() {
    if (feed_race_timer_)
        feed_race_timer_->stop();
    if (feed_race_service_)
        feed_race_service_->stop();
    if (feed_race_fresh_lbl_) {
        feed_race_fresh_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
        feed_race_fresh_lbl_->setText(tr("stopped"));
    }
}

void PolymarketDetailPanel::refresh_feed_race() {
    if (!feed_race_service_)
        return;
    if (!feed_race_service_->is_running()) {
        start_feed_race();
        return;
    }
    render_feed_race(feed_race_service_->snapshot());
}

void PolymarketDetailPanel::render_feed_race(const latency::CryptoLatencySnapshot& snapshot) {
    if (!feed_race_table_)
        return;

    render_microstructure(microstructure_model_.snapshot(snapshot));

    if (feed_race_fresh_lbl_) {
        const bool live = snapshot.freshest_age_ms >= 0;
        const QColor color = live ? QColor(colors::POSITIVE()) : QColor(colors::TEXT_DIM());
        feed_race_fresh_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: 700; background: transparent;")
                .arg(color.name()));
        feed_race_fresh_lbl_->setText(
            live ? tr("%1 freshest, %2ms, %3bps")
                       .arg(snapshot.freshest_source)
                       .arg(snapshot.freshest_age_ms)
                       .arg(snapshot.cross_source_spread_bps, 0, 'f', 2)
                 : tr("waiting for trades"));
    }

    QHash<QString, latency::CryptoLatencyTick> latest;
    for (const auto& t : snapshot.latest_ticks)
        latest.insert(t.source, t);

    feed_race_table_->setRowCount(snapshot.sources.size());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int row = 0; row < snapshot.sources.size(); ++row) {
        const auto& state = snapshot.sources[row];
        const auto tick = latest.value(state.source);
        const qint64 age = state.last_tick_ms > 0 ? now - state.last_tick_ms : -1;
        const QString price = tick.price > 0.0
                                  ? QString::number(tick.price, 'f', tick.price < 10.0 ? 4 : 2)
                                  : QStringLiteral("-");
        const QString bid = tick.best_bid > 0.0
                                ? QString::number(tick.best_bid, 'f', tick.best_bid < 10.0 ? 4 : 2)
                                : QStringLiteral("-");
        const QString ask = tick.best_ask > 0.0
                                ? QString::number(tick.best_ask, 'f', tick.best_ask < 10.0 ? 4 : 2)
                                : QStringLiteral("-");
        const QString msg = state.error.isEmpty()
                                ? (state.last_message_type.isEmpty() ? QStringLiteral("-") : state.last_message_type)
                                : state.error.left(24);

        const QStringList cells = {
            state.source,
            state.status,
            bid,
            ask,
            price,
            age >= 0 ? tr("%1ms").arg(age) : QStringLiteral("-"),
            QString::number(state.ticks),
            QStringLiteral("%1 %2").arg(state.raw_messages).arg(msg)
        };
        for (int col = 0; col < cells.size(); ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            item->setForeground(QColor(state.last_tick_ms > 0 ? colors::TEXT_PRIMARY() : colors::TEXT_SECONDARY()));
            if (col == 1 && state.status == QStringLiteral("live"))
                item->setForeground(QColor(colors::POSITIVE()));
            feed_race_table_->setItem(row, col, item);
        }
    }
    feed_race_table_->resizeColumnsToContents();
}

void PolymarketDetailPanel::render_microstructure(const edge::CryptoMicrostructureSnapshot& snapshot) {
    if (!micro_call_lbl_ || !micro_pressure_lbl_ || !micro_divergence_lbl_)
        return;
    const bool candidate = snapshot.call == QStringLiteral("TRADE CANDIDATE");
    const bool watch = snapshot.call == QStringLiteral("WATCH");
    const QColor color = candidate ? QColor(colors::POSITIVE())
                       : watch ? QColor(colors::WARNING())
                               : QColor(colors::TEXT_SECONDARY());
    micro_call_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 800; background: transparent;")
            .arg(color.name()));
    micro_call_lbl_->setText(QStringLiteral("%1 %2")
                                 .arg(snapshot.call, snapshot.direction.toUpper()));
    micro_pressure_lbl_->setText(
        tr("tape %1  book %2  conf %3%")
            .arg(snapshot.tape_pressure, 0, 'f', 2)
            .arg(snapshot.book_pressure, 0, 'f', 2)
            .arg(snapshot.confidence * 100.0, 0, 'f', 0));
    micro_divergence_lbl_->setText(
        tr("divergence %1bps  %2")
            .arg(snapshot.cross_source_spread_bps, 0, 'f', 2)
            .arg(snapshot.rationale));
}

void PolymarketDetailPanel::render_impulse(const edge::CryptoImpulseSignal& signal) {
    if (!impulse_move_lbl_ || !impulse_velocity_lbl_ || !impulse_call_lbl_)
        return;

    edge::CryptoImpulseWindow primary;
    for (const auto& w : signal.windows) {
        if (w.available && w.seconds == 15) {
            primary = w;
            break;
        }
    }
    if (!primary.available) {
        for (const auto& w : signal.windows) {
            if (w.available) {
                primary = w;
                break;
            }
        }
    }

    const bool pass = signal.gate == QStringLiteral("pass");
    const QColor call_color = pass ? QColor(colors::POSITIVE()) : QColor(colors::TEXT_SECONDARY());
    impulse_call_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 800; background: transparent;")
            .arg(call_color.name()));

    if (!primary.available) {
        impulse_move_lbl_->setText(tr("warming up"));
        impulse_velocity_lbl_->setText(tr("velocity -"));
        impulse_call_lbl_->setText(tr("no trade"));
        return;
    }

    const QColor move_color = primary.move_pct > 0.0 ? QColor(colors::POSITIVE())
                          : primary.move_pct < 0.0 ? QColor(colors::NEGATIVE())
                                                    : QColor(colors::TEXT_SECONDARY());
    impulse_move_lbl_->setStyleSheet(
        QString("color: %1; font-size: 11px; font-weight: 800; background: transparent;")
            .arg(move_color.name()));
    impulse_velocity_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));

    impulse_move_lbl_->setText(tr("%1 %2s %3")
                                   .arg(signal.symbol)
                                   .arg(primary.seconds)
                                   .arg(fmt_impulse_pct(primary.move_pct)));
    impulse_velocity_lbl_->setText(tr("%1 %2%/s confidence %3%")
                                       .arg(signal.strength)
                                       .arg(primary.velocity_pct_per_sec, 0, 'f', 4)
                                       .arg(signal.confidence * 100.0, 0, 'f', 0));
    impulse_call_lbl_->setText(pass ? tr("%1").arg(signal.recommendation)
                                    : tr("no trade"));
    impulse_call_lbl_->setToolTip(signal.rejection_reasons);
    apply_crypto_hourly_anchor();
    evaluate_edge_page();
}

QWidget* PolymarketDetailPanel::create_edge_page() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        QString("QScrollArea { border: none; background: %1; }"
                "QScrollBar:vertical { background: %1; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %2; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::BORDER_BRIGHT()));

    auto* page = new QWidget;
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(16, 14, 16, 16);
    vl->setSpacing(10);

    edge_market_lbl_ = new QLabel(tr("Select a market to score edge"));
    edge_market_lbl_->setWordWrap(true);
    edge_market_lbl_->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: 700; background: transparent;")
            .arg(colors::TEXT_PRIMARY()));
    vl->addWidget(edge_market_lbl_);

    edge_anchor_lbl_ = new QLabel(tr("BTC anchor: load Crypto + 1 Hour markets to auto-score hourly contracts"));
    edge_anchor_lbl_->setWordWrap(true);
    edge_anchor_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    vl->addWidget(edge_anchor_lbl_);

    auto* feed_box = new QWidget;
    feed_box->setStyleSheet(
        QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* fvl = new QVBoxLayout(feed_box);
    fvl->setContentsMargins(12, 10, 12, 10);
    fvl->setSpacing(8);

    auto* feed_top = new QWidget(feed_box);
    feed_top->setStyleSheet("background: transparent;");
    auto* ftl = new QHBoxLayout(feed_top);
    ftl->setContentsMargins(0, 0, 0, 0);
    ftl->setSpacing(8);
    auto* feed_title = make_edge_caption(tr("BTC MICROSTRUCTURE"));
    feed_race_symbol_lbl_ = new QLabel(tr("BTC-USD public exchange WebSockets"));
    feed_race_symbol_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 700; background: transparent;")
            .arg(colors::TEXT_PRIMARY()));
    feed_race_fresh_lbl_ = new QLabel(tr("not running"));
    feed_race_fresh_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    ftl->addWidget(feed_title);
    ftl->addWidget(feed_race_symbol_lbl_);
    ftl->addStretch(1);
    ftl->addWidget(feed_race_fresh_lbl_);
    fvl->addWidget(feed_top);

    feed_race_table_ = new QTableWidget(feed_box);
    feed_race_table_->setColumnCount(8);
    feed_race_table_->setHorizontalHeaderLabels(
        {tr("SOURCE"), tr("STATUS"), tr("BID"), tr("ASK"), tr("PRICE"), tr("AGE"), tr("TICKS"), tr("MSG")});
    feed_race_table_->verticalHeader()->setVisible(false);
    feed_race_table_->horizontalHeader()->setStretchLastSection(true);
    feed_race_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    feed_race_table_->setSelectionMode(QAbstractItemView::NoSelection);
    feed_race_table_->setFixedHeight(112);
    feed_race_table_->setStyleSheet(
        QString("QTableWidget { background: %1; color: %2; border: 1px solid %3; font-size: 10px; }"
                "QTableWidget::item { padding: 3px 6px; border-bottom: 1px solid %3; }"
                "QHeaderView::section { background: %4; color: %5; border: none;"
                "  border-bottom: 1px solid %3; padding: 4px 6px; font-size: 8px;"
                "  font-weight: 700; letter-spacing: 0.5px; }")
            .arg(colors::BG_BASE(), colors::TEXT_PRIMARY(), colors::BORDER_DIM(),
                 colors::BG_RAISED(), colors::TEXT_SECONDARY()));
    fvl->addWidget(feed_race_table_);

    auto* micro_row = new QWidget(feed_box);
    micro_row->setStyleSheet(
        QString("background: %1; border: 1px solid %2;")
            .arg(colors::BG_BASE(), colors::BORDER_DIM()));
    auto* mrl = new QHBoxLayout(micro_row);
    mrl->setContentsMargins(8, 6, 8, 6);
    mrl->setSpacing(10);
    mrl->addWidget(make_edge_caption(tr("RADAR")));
    micro_call_lbl_ = new QLabel(tr("not running"));
    micro_call_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 800; background: transparent;")
            .arg(colors::TEXT_SECONDARY()));
    micro_pressure_lbl_ = new QLabel(tr("tape - book -"));
    micro_pressure_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_PRIMARY()));
    micro_divergence_lbl_ = new QLabel(tr("divergence -"));
    micro_divergence_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    mrl->addWidget(micro_call_lbl_);
    mrl->addWidget(micro_pressure_lbl_);
    mrl->addStretch(1);
    mrl->addWidget(micro_divergence_lbl_);
    fvl->addWidget(micro_row);

    auto* impulse_row = new QWidget(feed_box);
    impulse_row->setStyleSheet(
        QString("background: %1; border: 1px solid %2;")
            .arg(colors::BG_BASE(), colors::BORDER_DIM()));
    auto* irl = new QHBoxLayout(impulse_row);
    irl->setContentsMargins(8, 6, 8, 6);
    irl->setSpacing(10);
    irl->addWidget(make_edge_caption(tr("IMPULSE")));
    impulse_move_lbl_ = new QLabel(tr("not running"));
    impulse_move_lbl_->setStyleSheet(
        QString("color: %1; font-size: 11px; font-weight: 800; background: transparent;")
            .arg(colors::TEXT_PRIMARY()));
    impulse_velocity_lbl_ = new QLabel(tr("velocity -"));
    impulse_velocity_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    impulse_call_lbl_ = new QLabel(tr("no trade"));
    impulse_call_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: 800; background: transparent;")
            .arg(colors::TEXT_SECONDARY()));
    irl->addWidget(impulse_move_lbl_);
    irl->addWidget(impulse_velocity_lbl_);
    irl->addStretch(1);
    irl->addWidget(impulse_call_lbl_);
    fvl->addWidget(impulse_row);

    auto* feed_buttons = new QWidget(feed_box);
    feed_buttons->setStyleSheet("background: transparent;");
    auto* fbl = new QHBoxLayout(feed_buttons);
    fbl->setContentsMargins(0, 0, 0, 0);
    fbl->setSpacing(8);
    feed_race_start_btn_ = new QPushButton(tr("START"));
    feed_race_stop_btn_ = new QPushButton(tr("STOP"));
    auto* refresh_btn = new QPushButton(tr("REFRESH"));
    for (auto* b : {feed_race_start_btn_, feed_race_stop_btn_, refresh_btn}) {
        b->setFixedHeight(28);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(edge_button_style(presentation_.accent, false));
    }
    connect(feed_race_start_btn_, &QPushButton::clicked, this, &PolymarketDetailPanel::start_feed_race);
    connect(feed_race_stop_btn_, &QPushButton::clicked, this, &PolymarketDetailPanel::stop_feed_race);
    connect(refresh_btn, &QPushButton::clicked, this, &PolymarketDetailPanel::refresh_feed_race);
    fbl->addWidget(feed_race_start_btn_);
    fbl->addWidget(feed_race_stop_btn_);
    fbl->addWidget(refresh_btn);
    fbl->addStretch(1);
    fvl->addWidget(feed_buttons);
    vl->addWidget(feed_box);

    auto* grid_box = new QWidget;
    grid_box->setStyleSheet(
        QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* grid = new QGridLayout(grid_box);
    grid->setContentsMargins(12, 10, 12, 10);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);

    auto add_spin = [&](int row, int col, const QString& label, QDoubleSpinBox*& target, double value) {
        auto* holder = new QWidget(grid_box);
        holder->setStyleSheet("background: transparent;");
        auto* hl = new QVBoxLayout(holder);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(3);
        hl->addWidget(make_edge_caption(label));
        target = make_edge_spin(value);
        hl->addWidget(target);
        grid->addWidget(holder, row, col);
        connect(target, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &PolymarketDetailPanel::evaluate_edge_page);
    };

    add_spin(0, 0, tr("MARKET"), edge_market_prob_, 0.0);
    add_spin(0, 1, tr("MODEL"), edge_model_prob_, 0.0);
    add_spin(0, 2, tr("SPREAD"), edge_spread_cost_, 0.0);
    add_spin(1, 0, tr("FEE"), edge_fee_cost_, 0.0);
    add_spin(1, 1, tr("LIQUIDITY"), edge_liquidity_, 60.0);
    add_spin(1, 2, tr("CONFIDENCE"), edge_confidence_, 60.0);
    vl->addWidget(grid_box);

    auto* result_box = new QWidget;
    result_box->setStyleSheet(
        QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* rl = new QGridLayout(result_box);
    rl->setContentsMargins(12, 10, 12, 10);
    rl->setHorizontalSpacing(12);
    rl->setVerticalSpacing(8);

    auto add_result = [&](int row, int col, const QString& caption, QLabel*& value) {
        auto* box = new QWidget(result_box);
        box->setStyleSheet("background: transparent;");
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(3);
        bl->addWidget(make_edge_caption(caption));
        value = new QLabel("—");
        value->setStyleSheet(
            QString("color: %1; font-size: 12px; font-weight: 800; background: transparent;")
                .arg(colors::TEXT_PRIMARY()));
        bl->addWidget(value);
        rl->addWidget(box, row, col);
    };

    add_result(0, 0, tr("SIDE"), edge_side_lbl_);
    add_result(0, 1, tr("RAW EDGE"), edge_raw_lbl_);
    add_result(0, 2, tr("NET EDGE"), edge_net_lbl_);
    add_result(1, 0, tr("CALL"), edge_reco_lbl_);
    add_result(1, 1, tr("RISK"), edge_risk_lbl_);
    add_result(1, 2, tr("GATE"), edge_gate_lbl_);
    vl->addWidget(result_box);

    vl->addWidget(make_edge_caption(tr("THESIS")));
    edge_thesis_ = new QTextEdit;
    edge_thesis_->setFixedHeight(82);
    edge_thesis_->setPlaceholderText(tr("Why your probability is different from the market"));
    edge_thesis_->setStyleSheet(edge_input_style());
    vl->addWidget(edge_thesis_);

    vl->addWidget(make_edge_caption(tr("RISK NOTES")));
    edge_risk_notes_ = new QTextEdit;
    edge_risk_notes_->setFixedHeight(64);
    edge_risk_notes_->setPlaceholderText(tr("What would invalidate this idea"));
    edge_risk_notes_->setStyleSheet(edge_input_style());
    vl->addWidget(edge_risk_notes_);

    auto* button_row = new QWidget;
    button_row->setStyleSheet("background: transparent;");
    auto* brl = new QHBoxLayout(button_row);
    brl->setContentsMargins(0, 0, 0, 0);
    brl->setSpacing(8);
    auto* eval_btn = new QPushButton(tr("EVALUATE"));
    auto* save_btn = new QPushButton(tr("SAVE EDGE"));
    for (auto* b : {eval_btn, save_btn}) {
        b->setFixedHeight(32);
        b->setCursor(Qt::PointingHandCursor);
    }
    eval_btn->setStyleSheet(edge_button_style(presentation_.accent, false));
    save_btn->setStyleSheet(edge_button_style(presentation_.accent, true));
    connect(eval_btn, &QPushButton::clicked, this, &PolymarketDetailPanel::evaluate_edge_page);
    connect(save_btn, &QPushButton::clicked, this, &PolymarketDetailPanel::save_edge_idea);
    brl->addWidget(eval_btn);
    brl->addWidget(save_btn);
    brl->addStretch(1);
    vl->addWidget(button_row);

    edge_status_lbl_ = new QLabel;
    edge_status_lbl_->setWordWrap(true);
    edge_status_lbl_->setStyleSheet(
        QString("color: %1; font-size: 10px; background: transparent;").arg(colors::TEXT_DIM()));
    vl->addWidget(edge_status_lbl_);

    vl->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

QWidget* PolymarketDetailPanel::create_holders_page() {
    holders_table_ = new QTableWidget;
    holders_table_->setColumnCount(4);
    holders_table_->setHorizontalHeaderLabels({tr("RANK"), tr("TRADER"), tr("SIZE"), tr("AVG PRICE")});
    holders_table_->horizontalHeader()->setStretchLastSection(true);
    holders_table_->verticalHeader()->setVisible(false);
    holders_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    holders_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    holders_table_->setShowGrid(false);
    holders_table_->setStyleSheet(
        QString("QTableWidget { background: %1; color: %2; border: none; font-size: 10px; }"
                "QTableWidget::item { padding: 4px 8px; border-bottom: 1px solid %3; }"
                "QTableWidget::item:selected { background: %4; color: %2; }"
                "QHeaderView::section {"
                "  background: %5; color: %6; border: none;"
                "  border-bottom: 1px solid %3;"
                "  padding: 5px 8px; font-size: 8px; font-weight: 700; letter-spacing: 0.5px;"
                "}"
                "QScrollBar:vertical { background: %1; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %3; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::TEXT_PRIMARY(), colors::BORDER_DIM(),
                 colors::BG_HOVER(), colors::BG_RAISED(), colors::TEXT_SECONDARY()));
    return holders_table_;
}

QWidget* PolymarketDetailPanel::create_comments_page() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        QString("QScrollArea { border: none; background: %1; }"
                "QScrollBar:vertical { background: %1; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %2; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::BORDER_BRIGHT()));
    comments_container_ = new QWidget;
    comments_container_->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(comments_container_);
    vl->setContentsMargins(16, 16, 16, 16);
    vl->setSpacing(8);
    auto* empty = new QLabel(tr("No comments yet"));
    empty->setStyleSheet(
        QString("color: %1; font-size: 12px; background: transparent;").arg(colors::TEXT_DIM()));
    empty->setAlignment(Qt::AlignCenter);
    vl->addWidget(empty);
    vl->addStretch(1);
    scroll->setWidget(comments_container_);
    return scroll;
}

QWidget* PolymarketDetailPanel::create_related_page() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        QString("QScrollArea { border: none; background: %1; }"
                "QScrollBar:vertical { background: %1; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %2; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::BORDER_BRIGHT()));
    related_container_ = new QWidget;
    related_container_->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(related_container_);
    vl->setContentsMargins(16, 16, 16, 16);
    vl->setSpacing(6);
    auto* empty = new QLabel(tr("No related markets"));
    empty->setStyleSheet(
        QString("color: %1; font-size: 12px; background: transparent;").arg(colors::TEXT_DIM()));
    empty->setAlignment(Qt::AlignCenter);
    vl->addWidget(empty);
    vl->addStretch(1);
    scroll->setWidget(related_container_);
    return scroll;
}

void PolymarketDetailPanel::set_active_tab(int tab) {
    stack_->setCurrentIndex(tab);
    for (int i = 0; i < tab_btns_.size(); ++i) {
        tab_btns_[i]->setProperty("active", i == tab);
        tab_btns_[i]->style()->unpolish(tab_btns_[i]);
        tab_btns_[i]->style()->polish(tab_btns_[i]);
    }
    emit tab_changed(tab);
}

// ── Data setters ────────────────────────────────────────────────────────────

void PolymarketDetailPanel::set_market(const PredictionMarket& market) {
    const int previous_tab = stack_ ? stack_->currentIndex() : 0;
    // A live price tick re-calls set_market for the SAME market ~1x/sec. Detect
    // that (vs a genuinely new selection) so a live update doesn't yank the user
    // off the tab they're on (TRADE/ORDER BOOK/CHART) back to OVERVIEW.
    const bool same_market = has_last_market_ &&
                             last_market_.key.market_id == market.key.market_id;
    last_market_ = market;
    has_last_market_ = true;

    question_label_->setText(market.question);
    volume_label_->setText(presentation_.format_volume(market.volume));
    liquidity_label_->setText(presentation_.format_liquidity(market.liquidity));
    end_date_label_->setText(market.end_date_iso.left(10));
    midpoint_label_->setText("—");
    spread_label_->setText("—");
    last_trade_label_->setText("—");
    oi_label_->setText("—");

    render_status_badge(market);

    // ── Populate ticket outcome combo ─────────────────────────────────────
    if (ticket_outcome_cb_) {
        ticket_outcome_cb_->clear();
        for (const auto& o : market.outcomes)
            ticket_outcome_cb_->addItem(o.name);
    }
    if (ticket_status_lbl_) ticket_status_lbl_->clear();
    if (ticket_submit_btn_) ticket_submit_btn_->setEnabled(true);
    if (ticket_position_lbl_) ticket_position_lbl_->setText("—");
    configure_ticket_order_types();
    prefill_ticket_price_from_outcome(!same_market);
    update_ticket_estimate();

    // ── Rebuild outcome probability bars ──────────────────────────────────
    auto* layout = qobject_cast<QVBoxLayout*>(outcome_container_->layout());
    while (layout->count() > 0) {
        auto* item = layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    for (int i = 0; i < market.outcomes.size(); ++i) {
        const auto& outcome = market.outcomes[i];
        const double pct = qBound(0.0, outcome.price, 1.0);
        const int pct_int = qRound(pct * 100.0);

        QColor bar_color;
        if (i == 0) bar_color = presentation_.accent;
        else if (i < 5) bar_color = QColor(OUTCOME_COLORS[i]);
        else bar_color = QColor(colors::TEXT_SECONDARY());

        // Outcome row: name + bar + price
        auto* row = new QWidget(outcome_container_);
        row->setStyleSheet("background: transparent;");
        auto* rl = new QVBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(3);

        // Top line: name + percentage
        auto* top_line = new QWidget(row);
        top_line->setStyleSheet("background: transparent;");
        auto* tll = new QHBoxLayout(top_line);
        tll->setContentsMargins(0, 0, 0, 0);
        tll->setSpacing(6);

        auto* name_lbl = new QLabel(outcome.name, top_line);
        name_lbl->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: 600; background: transparent;")
                .arg(colors::TEXT_PRIMARY()));

        auto* pct_lbl = new QLabel(QString("%1%").arg(pct_int), top_line);
        pct_lbl->setStyleSheet(
            QString("color: %1; font-size: 11px; font-weight: 700; background: transparent;")
                .arg(bar_color.name()));

        // Full formatted price (right side)
        auto* price_lbl = new QLabel(presentation_.format_price(pct), top_line);
        price_lbl->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: 600; background: transparent;")
                .arg(colors::TEXT_SECONDARY()));

        tll->addWidget(name_lbl);
        tll->addStretch(1);
        tll->addWidget(pct_lbl);
        tll->addSpacing(8);
        tll->addWidget(price_lbl);
        rl->addWidget(top_line);

        // Progress bar — a simple fixed-height widget
        auto* bar_track = new QWidget(row);
        bar_track->setFixedHeight(6);
        bar_track->setStyleSheet(
            QString("background: %1; border-radius: 0px;").arg(colors::BG_RAISED()));
        // Fill widget sits inside the track with proportional width — use
        // a layout that we stretch manually via resize.
        auto* bar_fill = new QWidget(bar_track);
        bar_fill->setStyleSheet(
            QString("background: %1; border-radius: 0px;").arg(bar_color.name()));
        // Store pct on the fill widget so it can be re-sized on layout.
        bar_fill->setProperty("fill_pct", pct);

        // We need the fill to track the track width — use a nested layout.
        auto* bar_layout = new QHBoxLayout(bar_track);
        bar_layout->setContentsMargins(0, 0, 0, 0);
        bar_layout->setSpacing(0);
        bar_layout->addWidget(bar_fill, qRound(pct * 1000));
        if (pct < 1.0) bar_layout->addStretch(qRound((1.0 - pct) * 1000));

        rl->addWidget(bar_track);
        layout->addWidget(row);
    }

    description_label_->setText(market.description.left(600));

    QStringList labels;
    labels.reserve(market.outcomes.size());
    for (const auto& o : market.outcomes)
        labels.append(o.name);
    price_chart_->set_outcome_labels(labels);
    update_edge_prefill_from_market();

    // Same market updating -> keep the user's current tab; a genuinely new
    // market -> reset to OVERVIEW (EDGE stays sticky in both cases).
    set_active_tab(same_market ? previous_tab
                               : (previous_tab == kTabEdge ? kTabEdge : 0));
}

void PolymarketDetailPanel::set_price_summary(const pmx::PriceSummary& summary) {
    midpoint_label_->setText(presentation_.format_price(summary.midpoint));
    spread_label_->setText(presentation_.format_price(summary.spread));
    last_trade_label_->setText(presentation_.format_price(summary.last_trade_price));
}

void PolymarketDetailPanel::set_order_book(const PredictionOrderBook& book) {
    orderbook_->set_data(book);
    if (edge_spread_cost_ && !book.bids.isEmpty() && !book.asks.isEmpty()) {
        const double best_bid = book.bids.first().price;
        const double best_ask = book.asks.first().price;
        const double spread = qBound(0.0, best_ask - best_bid, 1.0);
        const QSignalBlocker block(edge_spread_cost_);
        edge_spread_cost_->setValue(spread * 100.0);
        evaluate_edge_page();
    }
}

void PolymarketDetailPanel::set_price_history(const PriceHistory& history) {
    price_chart_->set_price_history(history);
}

void PolymarketDetailPanel::set_trades(const QVector<PredictionTrade>& trades) {
    activity_feed_->set_trades(trades);
}

void PolymarketDetailPanel::set_top_holders(const QVector<pmx::TopHolder>& holders) {
    holders_table_->setSortingEnabled(false);
    holders_table_->setRowCount(holders.size());
    for (int i = 0; i < holders.size(); ++i) {
        const pmx::TopHolder& h = holders[i];
        auto* rank = new QTableWidgetItem(QString::number(h.rank > 0 ? h.rank : i + 1));
        rank->setTextAlignment(Qt::AlignCenter);
        holders_table_->setItem(i, 0, rank);
        holders_table_->setItem(i, 1, new QTableWidgetItem(h.display_name));
        auto* size_item = new QTableWidgetItem(QString::number(h.position_size, 'f', 2));
        size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        holders_table_->setItem(i, 2, size_item);
        auto* price_item = new QTableWidgetItem(QString::number(h.entry_price, 'f', 4));
        price_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        holders_table_->setItem(i, 3, price_item);
        if (i % 2 == 1) {
            for (int c = 0; c < 4; ++c) {
                if (auto* it = holders_table_->item(i, c))
                    it->setBackground(QColor(colors::ROW_ALT()));
            }
        }
    }
    holders_table_->resizeColumnsToContents();
    holders_table_->setSortingEnabled(true);
}

void PolymarketDetailPanel::set_comments(const QVector<pmx::Comment>& comments) {
    auto* vl = qobject_cast<QVBoxLayout*>(comments_container_->layout());
    while (vl->count() > 0) {
        auto* item = vl->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (comments.isEmpty()) {
        auto* empty = new QLabel(tr("No comments yet"));
        empty->setStyleSheet(
            QString("color: %1; font-size: 12px; background: transparent;").arg(colors::TEXT_DIM()));
        empty->setAlignment(Qt::AlignCenter);
        vl->addWidget(empty);
    } else {
        for (const auto& c : comments) {
            auto* card = new QWidget(comments_container_);
            card->setStyleSheet(
                QString("background: %1; border: 1px solid %2; border-left: 2px solid %3;")
                    .arg(colors::BG_SURFACE(), colors::BORDER_DIM(), colors::AMBER()));
            auto* cvl = new QVBoxLayout(card);
            cvl->setContentsMargins(10, 8, 10, 8);
            cvl->setSpacing(4);

            auto* author = new QLabel(c.author.isEmpty() ? c.author_address.left(12) + "…" : c.author);
            author->setStyleSheet(
                QString("color: %1; font-size: 9px; font-weight: 700; background: transparent;")
                    .arg(colors::AMBER()));

            auto* body = new QLabel(c.body.left(280));
            body->setStyleSheet(
                QString("color: %1; font-size: 10px; background: transparent; line-height: 1.4;")
                    .arg(colors::TEXT_PRIMARY()));
            body->setWordWrap(true);

            auto* meta = new QLabel(
                QDateTime::fromSecsSinceEpoch(c.created_at, QTimeZone::UTC).toString("yyyy-MM-dd HH:mm") +
                (c.likes > 0 ? tr("  · %1 likes").arg(c.likes) : QString()));
            meta->setStyleSheet(
                QString("color: %1; font-size: 8px; background: transparent;").arg(colors::TEXT_DIM()));

            cvl->addWidget(author);
            cvl->addWidget(body);
            cvl->addWidget(meta);
            vl->addWidget(card);
        }
    }
    vl->addStretch(1);
}

void PolymarketDetailPanel::set_related_markets(const QVector<PredictionMarket>& markets) {
    auto* vl = qobject_cast<QVBoxLayout*>(related_container_->layout());
    while (vl->count() > 0) {
        auto* item = vl->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (markets.isEmpty()) {
        auto* empty = new QLabel(tr("No related markets"));
        empty->setStyleSheet(
            QString("color: %1; font-size: 12px; background: transparent;").arg(colors::TEXT_DIM()));
        empty->setAlignment(Qt::AlignCenter);
        vl->addWidget(empty);
    } else {
        for (const auto& m : markets) {
            auto* card = new QPushButton(related_container_);
            card->setStyleSheet(
                QString("QPushButton {"
                        "  background: %1;"
                        "  color: %2;"
                        "  border: 1px solid %3;"
                        "  text-align: left;"
                        "  padding: 8px 12px;"
                        "  font-size: 10px;"
                        "  font-weight: 600;"
                        "}"
                        "QPushButton:hover { background: %4; color: %5; border-color: %6; }")
                    .arg(colors::BG_SURFACE(), colors::TEXT_SECONDARY(), colors::BORDER_DIM(),
                         colors::BG_HOVER(), colors::TEXT_PRIMARY(), colors::BORDER_BRIGHT()));
            card->setText(m.question.left(70) + (m.question.size() > 70 ? "…" : ""));
            card->setCursor(Qt::PointingHandCursor);
            connect(card, &QPushButton::clicked, this, [this, m]() { emit related_market_clicked(m); });
            vl->addWidget(card);
        }
    }
    vl->addStretch(1);
}

void PolymarketDetailPanel::set_open_interest(double oi) {
    oi_label_->setText(presentation_.format_volume(oi));
}

void PolymarketDetailPanel::set_series_tooltip(const QString& tooltip) {
    if (question_label_) question_label_->setToolTip(tooltip);
}

void PolymarketDetailPanel::set_polymarket_extras_enabled(bool enabled) {
    const int extra_tabs[] = {kTabHolders, kTabComments, kTabRelated};
    for (int idx : extra_tabs) {
        if (idx >= 0 && idx < tab_btns_.size())
            tab_btns_[idx]->setVisible(enabled);
    }
    if (!enabled) {
        if (holders_table_) holders_table_->setRowCount(0);

        auto clear_container = [](QWidget* container, const QString& msg) {
            if (!container) return;
            auto* vl = qobject_cast<QVBoxLayout*>(container->layout());
            if (!vl) return;
            while (vl->count() > 0) {
                auto* item = vl->takeAt(0);
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            auto* empty = new QLabel(msg);
            empty->setStyleSheet(
                QString("color: %1; font-size: 12px; background: transparent;")
                    .arg(colors::TEXT_DIM()));
            empty->setAlignment(Qt::AlignCenter);
            vl->addWidget(empty);
            vl->addStretch(1);
        };
        clear_container(comments_container_, tr("Comments are Polymarket-only"));
        clear_container(related_container_, tr("Related markets are Polymarket-only"));

        const int current = stack_ ? stack_->currentIndex() : 0;
        if (current == kTabHolders || current == kTabComments || current == kTabRelated)
            set_active_tab(0);
    }
}

void PolymarketDetailPanel::clear() {
    has_last_market_ = false;
    last_market_ = {};
    question_label_->setText(tr("Select a market to view details"));
    volume_label_->setText("—");
    liquidity_label_->setText("—");
    end_date_label_->setText("—");
    midpoint_label_->setText("—");
    spread_label_->setText("—");
    last_trade_label_->setText("—");
    oi_label_->setText("—");
    status_label_->clear();
    description_label_->clear();
    orderbook_->clear();
    activity_feed_->clear();
    if (edge_market_lbl_) {
        edge_market_lbl_->setProperty("market_id", QString{});
        edge_market_lbl_->setText(tr("Select a market to score edge"));
    }
    if (edge_market_prob_) edge_market_prob_->setValue(0.0);
    if (edge_model_prob_) edge_model_prob_->setValue(0.0);
    if (edge_spread_cost_) edge_spread_cost_->setValue(0.0);
    if (edge_side_lbl_) edge_side_lbl_->setText("—");
    if (edge_raw_lbl_) edge_raw_lbl_->setText("—");
    if (edge_net_lbl_) edge_net_lbl_->setText("—");
    if (edge_reco_lbl_) edge_reco_lbl_->setText("—");
    if (edge_risk_lbl_) edge_risk_lbl_->setText("—");
    if (edge_status_lbl_) edge_status_lbl_->clear();

    // Clear outcomes
    if (auto* vl = qobject_cast<QVBoxLayout*>(outcome_container_->layout())) {
        while (vl->count() > 0) {
            auto* item = vl->takeAt(0);
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }
}

// ── Presentation wiring ─────────────────────────────────────────────────────

void PolymarketDetailPanel::set_presentation(const ExchangePresentation& p) {
    const bool accent_changed = p.accent != presentation_.accent;
    const bool oi_changed = p.has_open_interest != presentation_.has_open_interest;
    const bool extras_changed = p.has_polymarket_extras != presentation_.has_polymarket_extras;

    presentation_ = p;

    if (accent_changed) apply_accent_to_tabs();
    if (oi_changed) apply_presentation_to_stats();
    if (extras_changed) set_polymarket_extras_enabled(p.has_polymarket_extras);

    if (has_last_market_) set_market(last_market_);
}

void PolymarketDetailPanel::apply_accent_to_tabs() {
    const QColor& a = presentation_.accent;
    const QString accent_str =
        QStringLiteral("rgba(%1,%2,%3,1.0)").arg(a.red()).arg(a.green()).arg(a.blue());
    const QString accent_bg =
        QStringLiteral("rgba(%1,%2,%3,0.12)").arg(a.red()).arg(a.green()).arg(a.blue());

    const QString css =
        QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  color: %1;"
            "  border: none;"
            "  border-bottom: 2px solid transparent;"
            "  font-size: 9px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.5px;"
            "  padding: 0 14px;"
            "  min-height: 34px;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "  border-bottom-color: %3;"
            "}"
            "QPushButton[active=\"true\"] {"
            "  color: %4;"
            "  border-bottom-color: %4;"
            "  background: %5;"
            "}")
        .arg(colors::TEXT_SECONDARY())  // inactive text
        .arg(colors::TEXT_PRIMARY())    // hover text
        .arg(colors::BORDER_BRIGHT())   // hover underline
        .arg(accent_str)                // active text + underline
        .arg(accent_bg);                // active background tint

    for (auto* btn : tab_btns_) {
        btn->setStyleSheet(css);
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
    }
}

void PolymarketDetailPanel::apply_presentation_to_stats() {
    if (oi_box_) oi_box_->setVisible(presentation_.has_open_interest);
}

void PolymarketDetailPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void PolymarketDetailPanel::retranslateUi() {
    // Tab bar labels (logical tab is index-based; labels are display-only).
    const QStringList tab_names = {
        tr("OVERVIEW"), tr("ORDER BOOK"), tr("CHART"), tr("TRADE"),
        tr("TRADES"), tr("EDGE"), tr("HOLDERS"), tr("COMMENTS"), tr("RELATED")
    };
    for (int i = 0; i < tab_btns_.size() && i < tab_names.size(); ++i)
        tab_btns_[i]->setText(tab_names[i]);

    // Overview stat captions (declared order matches make_stat_in calls).
    const QStringList stat_caps = {
        tr("VOLUME"), tr("LIQUIDITY"), tr("OPEN INT"), tr("END DATE"),
        tr("MIDPOINT"), tr("SPREAD"), tr("LAST TRADE")
    };
    for (int i = 0; i < stat_caption_lbls_.size() && i < stat_caps.size(); ++i)
        if (stat_caption_lbls_[i]) stat_caption_lbls_[i]->setText(stat_caps[i]);

    if (outcomes_header_) outcomes_header_->setText(tr("OUTCOMES"));

    // Trade page captions.
    if (no_acct_msg_lbl_) no_acct_msg_lbl_->setText(tr("Connect an account\nto place orders"));
    if (bal_caption_lbl_) bal_caption_lbl_->setText(tr("AVAILABLE"));
    if (pos_caption_lbl_) pos_caption_lbl_->setText(tr("POSITION"));
    const QStringList form_caps = {
        tr("OUTCOME"),
        tr("DOLLAR AMOUNT"),
        tr("LIMIT PRICE"),
        tr("CONTRACTS"),
        tr("ORDER DURATION")
    };
    for (int i = 0; i < trade_form_caption_lbls_.size() && i < form_caps.size(); ++i)
        if (trade_form_caption_lbls_[i]) trade_form_caption_lbls_[i]->setText(form_caps[i]);
    if (ticket_buy_btn_)    ticket_buy_btn_->setText(tr("BUY"));
    if (ticket_sell_btn_)   ticket_sell_btn_->setText(tr("SELL"));
    if (ticket_submit_btn_) ticket_submit_btn_->setText(tr("PLACE ORDER"));
    configure_ticket_order_types();

    // Holders table headers.
    if (holders_table_)
        holders_table_->setHorizontalHeaderLabels({tr("RANK"), tr("TRADER"), tr("SIZE"), tr("AVG PRICE")});

    // Re-render question/badge/outcome bars for the current market so the
    // status badge text picks up the new language. Per-row data refreshes
    // through the normal data path.
    if (has_last_market_) {
        render_status_badge(last_market_);
    } else if (question_label_) {
        question_label_->setText(tr("Select a market to view details"));
    }
}

void PolymarketDetailPanel::render_status_badge(const PredictionMarket& market) {
    const auto badge = presentation_.status_badge(market);
    status_label_->setText(badge.text);
    status_label_->setToolTip(badge.tooltip);

    const QString bg_css = badge.bg.alpha() > 0
        ? QStringLiteral("rgba(%1,%2,%3,%4)")
              .arg(badge.bg.red()).arg(badge.bg.green()).arg(badge.bg.blue())
              .arg(badge.bg.alphaF(), 0, 'f', 2)
        : QStringLiteral("transparent");

    status_label_->setStyleSheet(
        QString("color: %1; background: %2; font-size: 8px; font-weight: 700; "
                "padding: 2px 8px; border: 1px solid %3; letter-spacing: 0.5px;")
            .arg(badge.fg.name(), bg_css, badge.fg.name()));
}

} // namespace openmarketterminal::screens::polymarket
