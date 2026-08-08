#include "screens/weather/WeatherScreen.h"

#include "cli/ServeCommand.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPair>
#include <QPolygon>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

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

// Task 5 bot panel — ported verbatim (query pattern + presentation) from the
// retired lean weather predictions screen.
int q_int(const QString& sql) {
    auto r = Database::instance().execute(sql, {});
    if (!r.is_ok() || !r.value().next())
        return 0;
    return r.value().value(0).toInt();
}

double q_double(const QString& sql) {
    auto r = Database::instance().execute(sql, {});
    if (!r.is_ok() || !r.value().next() || r.value().value(0).isNull())
        return 0.0;
    return r.value().value(0).toDouble();
}

QLabel* make_stat(QLabel** value_out, const QString& caption, QVBoxLayout* into) {
    auto* box = new QVBoxLayout;
    auto* cap = new QLabel(caption);
    cap->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;background:transparent;%2")
                           .arg(ui::colors::TEXT_TERTIARY(), MF));
    auto* val = new QLabel(QStringLiteral("-"));
    val->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                           .arg(ui::colors::TEXT_PRIMARY(), MF));
    box->addWidget(cap);
    box->addWidget(val);
    box->setSpacing(0);
    into->addLayout(box);
    *value_out = val;
    return val;
}

double outcome_price(const pred::PredictionMarket& market, int index) {
    return index >= 0 && index < market.outcomes.size() ? market.outcomes[index].price : 0.0;
}

// Task 7: order-entry formatting helpers (mirror KalshiScreen's anonymous
// namespace money()/probability() so the paper preview reads the same way
// the live ticket's preview does).
QString order_money(double value) { return QStringLiteral("$%1").arg(value, 0, 'f', 2); }
QString order_probability(double value) { return QStringLiteral("%1%").arg(std::round(value * 100.0), 0, 'f', 0); }

/// The six weather series the bot actually trades (weather_producer.py's
/// WEATHER_CITIES table). Tier 1: the browser is filtered down to exactly
/// these — the "Climate and Weather" category the fetch resolves to also
/// carries series the bot never touches (other cities, non-high-temp
/// products), and those would otherwise drown out the bot's own cities.
const QHash<QString, QString>& known_weather_series() {
    static const QHash<QString, QString> kKnownSeries = {
        {QStringLiteral("KXHIGHNY"), QStringLiteral("NYC")},
        {QStringLiteral("KXHIGHCHI"), QStringLiteral("CHI")},
        {QStringLiteral("KXHIGHDEN"), QStringLiteral("DEN")},
        {QStringLiteral("KXHIGHTSFO"), QStringLiteral("SFO")},
        {QStringLiteral("KXHIGHPHIL"), QStringLiteral("PHIL")},
        {QStringLiteral("KXHIGHTSEA"), QStringLiteral("SEA")},
    };
    return kKnownSeries;
}

/// City abbreviation from the series ticker prefix (e.g. "KXHIGHNY" for the
/// NYC daily-high-temperature series). Unknown prefixes fall back to the raw
/// series ticker so a new/unmapped city still shows something identifiable
/// rather than a blank cell.
QString city_for_series(const QString& series_ticker) {
    return known_weather_series().value(series_ticker, series_ticker);
}

/// Tier 1: true only for the six series the bot trades — the display filter
/// applied to whatever the "Climate and Weather" category fetch returns.
bool is_bot_weather_series(const QString& series_ticker) {
    return known_weather_series().contains(series_ticker);
}

/// Small forecast-vs-threshold visual (Tier 3): the forecast high plotted
/// against the selected bracket's strike(s) on a horizontal number line,
/// rather than as bare numbers. Self-contained — computes its own axis range
/// from whatever data it is given and repaints on set_data()/clear_data().
class ForecastNumberLine : public QWidget {
  public:
    explicit ForecastNumberLine(QWidget* parent = nullptr) : QWidget(parent) {
        // Tall enough for the forecast label above the axis and the
        // floor/cap strike labels below it without either clipping against
        // the widget edge (see paintEvent: label rows are anchored off
        // height(), not off a fixed offset from axis_y).
        setFixedHeight(64);
        setMinimumWidth(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void clear_data() {
        has_forecast_ = false;
        has_floor_ = false;
        has_cap_ = false;
        update();
    }

    void set_data(double forecast_high, bool has_floor, double floor_strike, bool has_cap, double cap_strike,
                  bool has_threshold, bool meets_threshold) {
        has_forecast_ = true;
        forecast_high_ = forecast_high;
        has_floor_ = has_floor;
        floor_strike_ = floor_strike;
        has_cap_ = has_cap;
        cap_strike_ = cap_strike;
        has_threshold_ = has_threshold;
        meets_threshold_ = meets_threshold;

        double lo = forecast_high, hi = forecast_high;
        if (has_floor_) { lo = std::min(lo, floor_strike_); hi = std::max(hi, floor_strike_); }
        if (has_cap_) { lo = std::min(lo, cap_strike_); hi = std::max(hi, cap_strike_); }
        const double pad = std::max(4.0, (hi - lo) * 0.35);
        axis_lo_ = lo - pad;
        axis_hi_ = hi + pad;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(QFont(QStringLiteral("Consolas"), 8));

        const int margin = 10;
        const int axis_y = height() / 2 + 4;
        const int x0 = margin, x1 = width() - margin;

        painter.setPen(QPen(QColor(ui::colors::BORDER_DIM()), 1));
        painter.drawLine(x0, axis_y, x1, axis_y);

        if (!has_forecast_ || axis_hi_ <= axis_lo_) {
            painter.setPen(QColor(ui::colors::TEXT_TERTIARY()));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("no forecast yet"));
            return;
        }

        auto to_x = [&](double value) {
            const double t = (value - axis_lo_) / (axis_hi_ - axis_lo_);
            return x0 + static_cast<int>(t * (x1 - x0));
        };

        // Threshold band/marks first so the forecast marker paints on top.
        // Strike labels are anchored off height() (not axis_y + a fixed
        // offset) so they always land inside the widget regardless of its
        // fixed height.
        const int label_y = height() - 13;
        painter.setPen(QPen(QColor(ui::colors::TEXT_SECONDARY()), 1, Qt::DashLine));
        if (has_floor_) {
            const int x = to_x(floor_strike_);
            painter.drawLine(x, axis_y - 8, x, axis_y + 8);
            painter.drawText(x - 20, label_y, 40, 12, Qt::AlignHCenter,
                              QString::asprintf("%.0f", floor_strike_));
        }
        if (has_cap_) {
            const int x = to_x(cap_strike_);
            painter.drawLine(x, axis_y - 8, x, axis_y + 8);
            painter.drawText(x - 20, label_y, 40, 12, Qt::AlignHCenter,
                              QString::asprintf("%.0f", cap_strike_));
        }

        const QColor marker_color = !has_threshold_  ? QColor(ui::colors::TEXT_TERTIARY())
                                     : meets_threshold_ ? QColor(ui::colors::POSITIVE())
                                                        : QColor(ui::colors::NEGATIVE());
        const int fx = to_x(forecast_high_);
        painter.setPen(QPen(marker_color, 2));
        painter.setBrush(marker_color);
        const QPolygon diamond({QPoint(fx, axis_y - 9), QPoint(fx + 6, axis_y), QPoint(fx, axis_y + 9),
                                 QPoint(fx - 6, axis_y)});
        painter.drawPolygon(diamond);
        painter.setPen(marker_color);
        painter.drawText(fx - 30, axis_y - 22, 60, 12, Qt::AlignHCenter,
                          QString::asprintf("%.1f°F", forecast_high_));
    }

  private:
    bool has_forecast_ = false;
    double forecast_high_ = 0.0;
    bool has_floor_ = false;
    double floor_strike_ = 0.0;
    bool has_cap_ = false;
    double cap_strike_ = 0.0;
    bool has_threshold_ = false;
    bool meets_threshold_ = false;
    double axis_lo_ = 0.0;
    double axis_hi_ = 1.0;
};

} // namespace

WeatherScreen::WeatherScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    wire_adapter();

    // Task 5: bot panel periodic refresh while visible (mirrors the retired
    // lean predictions screen's 30s cadence).
    bot_timer_ = new QTimer(this);
    bot_timer_->setInterval(30'000);
    connect(bot_timer_, &QTimer::timeout, this, &WeatherScreen::refresh_bot_panel);

    // Tier 1: backstop for a fetch whose reply never arrives at all (no
    // events_ready/markets_ready, no error_occurred) — without this a broken
    // connection leaves the browser on "Loading weather brackets…" forever.
    load_timeout_timer_ = new QTimer(this);
    load_timeout_timer_->setSingleShot(true);
    load_timeout_timer_->setInterval(12'000);
    connect(load_timeout_timer_, &QTimer::timeout, this, &WeatherScreen::handle_fetch_timeout);
}

void WeatherScreen::build_ui() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto* title = new QLabel(tr("KALSHI WEATHER — daily city high-temp brackets"));
    title->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                             .arg(ui::colors::AMBER(), MF));
    root->addWidget(title);

    auto* subtitle = new QLabel(tr("Every evaluated bracket across the weather cities. "
                                   "Separate from Bitcoin predictions; both run in parallel."));
    subtitle->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                .arg(ui::colors::TEXT_TERTIARY(), MF));
    root->addWidget(subtitle);

    // Task 5: main_tabs_ hosts the existing bracket browser/detail content
    // (unchanged, just reparented into a "BRACKETS" tab) alongside a new
    // "BOT" tab — the sub-tab option from the Task 5 spec.
    main_tabs_ = new QTabWidget(this);
    main_tabs_->setStyleSheet(QString(
        "QTabWidget::pane{border:1px solid %1;background:%2;}"
        "QTabBar::tab{background:%2;color:%3;padding:6px 16px;font-size:10px;font-weight:700;%4}"
        "QTabBar::tab:selected{background:%5;color:%6;}")
        .arg(ui::colors::BORDER_DIM(), ui::colors::BG_BASE(), ui::colors::TEXT_SECONDARY(), MF,
             ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY()));

    auto* brackets_tab = new QWidget(main_tabs_);
    auto* brackets_layout = new QVBoxLayout(brackets_tab);
    brackets_layout->setContentsMargins(0, 8, 0, 0);
    brackets_layout->setSpacing(10);

    auto* header_row = new QHBoxLayout;
    status_label_ = new QLabel(QStringLiteral("-"));
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), MF));
    header_row->addWidget(status_label_);
    header_row->addStretch();
    count_label_ = new QLabel(QStringLiteral("0 BRACKETS"));
    count_label_->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                    .arg(ui::colors::CYAN(), MF));
    header_row->addWidget(count_label_);
    refresh_button_ = new QPushButton(tr("REFRESH"));
    refresh_button_->setStyleSheet(QString(
        "QPushButton{background:%1;color:%2;border:1px solid %3;padding:2px 10px;font-size:10px;font-weight:700;%4}"
        "QPushButton:hover{color:%5;border-color:%5;}")
        .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::CYAN()));
    connect(refresh_button_, &QPushButton::clicked, this, &WeatherScreen::refresh);
    header_row->addWidget(refresh_button_);
    brackets_layout->addLayout(header_row);

    // Tier 3: P&L summary strip — "positions · settled · net P&L · win%" —
    // read from the same sandbox_position/sandbox_strategy tables the BOT
    // tab's stat row queries (populate_bot_summary() is the single source),
    // so the bot's scorecard is visible without switching tabs.
    auto* pnl_strip = new QHBoxLayout;
    pnl_strip->setSpacing(24);
    {
        auto add = [&](QLabel** out, const QString& cap) {
            auto* col = new QVBoxLayout;
            make_stat(out, cap, col);
            pnl_strip->addLayout(col);
        };
        add(&pnl_strip_positions_, tr("POSITIONS"));
        add(&pnl_strip_settled_, tr("SETTLED"));
        add(&pnl_strip_net_, tr("NET P&L (paper)"));
        add(&pnl_strip_winrate_, tr("WIN%"));
    }
    pnl_strip->addStretch();
    brackets_layout->addLayout(pnl_strip);

    auto* content_row = new QHBoxLayout;
    content_row->setSpacing(10);

    auto* left_col = new QVBoxLayout;
    auto* browser_hdr = new QLabel(tr("BRACKET BROWSER"));
    browser_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                   .arg(ui::colors::CYAN(), MF));
    left_col->addWidget(browser_hdr);

    bracket_table_ = new QTableWidget(0, 6, this);
    bracket_table_->setHorizontalHeaderLabels(
        {tr("City"), tr("Bracket"), tr("Mkt (mid)"), tr("Vol"), tr("Fcst-P"), tr("Edge")});
    bracket_table_->setStyleSheet(table_style());
    bracket_table_->verticalHeader()->setVisible(false);
    bracket_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bracket_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bracket_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    bracket_table_->horizontalHeader()->setStretchLastSection(true);
    bracket_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(bracket_table_, &QTableWidget::currentCellChanged, this,
            [this](int current_row, int, int, int) { select_bracket(current_row); });
    left_col->addWidget(bracket_table_, 1);
    content_row->addLayout(left_col, 3);

    // Right-side detail pane (Task 3): order book + recent trades for the
    // row selected in the browser. CryptoOrderBook is reused as-is (not
    // modified) — it renders generic price/size bid-ask pairs and already
    // takes no crypto-specific data through set_data(), the same widget
    // KalshiScreen embeds for its own reference DOM.
    auto* right_col = new QVBoxLayout;
    detail_title_ = new QLabel(tr("Select a bracket to view its order book and recent trades."));
    detail_title_->setWordWrap(true);
    detail_title_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
                                     .arg(ui::colors::TEXT_PRIMARY(), MF));
    right_col->addWidget(detail_title_);

    // Task 4: forecast-vs-market panel — the producer's evidence for the
    // selected bracket (forecast high/prob, live market mid, edge, the
    // in-window trading-lead badge, and a forecast-vs-strike indicator).
    auto* forecast_hdr = new QLabel(tr("FORECAST VS MARKET"));
    forecast_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                    .arg(ui::colors::CYAN(), MF));
    right_col->addWidget(forecast_hdr);

    auto* forecast_row = new QHBoxLayout;
    forecast_stats_label_ = new QLabel(tr("Select a bracket for its forecast."));
    forecast_stats_label_->setWordWrap(true);
    forecast_stats_label_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                             .arg(ui::colors::TEXT_PRIMARY(), MF));
    forecast_row->addWidget(forecast_stats_label_, 1);

    forecast_edge_label_ = new QLabel(QStringLiteral("EDGE —"));
    forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                            .arg(ui::colors::TEXT_TERTIARY(), MF));
    forecast_row->addWidget(forecast_edge_label_);

    forecast_window_badge_ = new QLabel(QStringLiteral("—"));
    forecast_window_badge_->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %3;padding:2px 6px;font-size:9px;font-weight:800;%4")
                                              .arg(ui::colors::TEXT_TERTIARY(), ui::colors::BG_RAISED(),
                                                   ui::colors::BORDER_DIM(), MF));
    forecast_row->addWidget(forecast_window_badge_);
    right_col->addLayout(forecast_row);

    forecast_threshold_label_ = new QLabel(tr(" "));
    forecast_threshold_label_->setWordWrap(true);
    forecast_threshold_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                                 .arg(ui::colors::TEXT_SECONDARY(), MF));
    right_col->addWidget(forecast_threshold_label_);

    // Tier 3: forecast-vs-market as a small visual — the forecast high
    // plotted against the selected bracket's threshold(s) on a horizontal
    // number line, updated alongside the text stats above in
    // update_forecast_panel().
    auto* forecast_line = new ForecastNumberLine(this);
    forecast_line_widget_ = forecast_line;
    right_col->addWidget(forecast_line_widget_);

    // Task 7: paper-only order-entry mini form for the selected bracket.
    build_order_entry_panel(right_col);

    order_book_widget_ = new crypto::CryptoOrderBook(this);
    right_col->addWidget(order_book_widget_, 2);

    auto* trades_hdr = new QLabel(tr("RECENT TRADES"));
    trades_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                  .arg(ui::colors::CYAN(), MF));
    right_col->addWidget(trades_hdr);

    trades_table_ = new QTableWidget(0, 4, this);
    trades_table_->setHorizontalHeaderLabels({tr("Time"), tr("Side"), tr("Price"), tr("Size")});
    trades_table_->setStyleSheet(table_style());
    trades_table_->verticalHeader()->setVisible(false);
    trades_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trades_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    trades_table_->horizontalHeader()->setStretchLastSection(true);
    right_col->addWidget(trades_table_, 1);

    content_row->addLayout(right_col, 2);

    brackets_layout->addLayout(content_row, 1);

    main_tabs_->addTab(brackets_tab, tr("BRACKETS"));
    main_tabs_->addTab(build_bot_tab(), tr("BOT"));
    // Tier 3: never elide the tab labels — narrow layouts otherwise render
    // "BRACKETS"/"BOT" as truncated "BRAC…"/"B…".
    main_tabs_->tabBar()->setElideMode(Qt::ElideNone);

    root->addWidget(main_tabs_, 1);
}

QWidget* WeatherScreen::build_bot_tab() {
    // Task 5 bot panel — summary (open/closed counts, realized PnL,
    // edge-status badge), decisions table, positions table. Ported verbatim
    // (query pattern, column layout) from the retired lean weather
    // predictions screen; filtered to sandbox_strategy kind='kalshi_weather'
    // and edge_decision_journal source='kalshi weather-plan'.
    auto* tab = new QWidget(main_tabs_);
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);

    auto* stats = new QHBoxLayout;
    stats->setSpacing(28);
    {
        auto add = [&](QLabel** out, const QString& cap) {
            auto* col = new QVBoxLayout;
            make_stat(out, cap, col);
            stats->addLayout(col);
        };
        add(&bot_open_count_, tr("OPEN POSITIONS"));
        add(&bot_resolved_count_, tr("RESOLVED"));
        add(&bot_win_count_, tr("WINS"));
        add(&bot_net_pnl_, tr("NET P&L (paper)"));
    }
    stats->addStretch();
    layout->addLayout(stats);

    // Edge-status badge — same lane-status text the retired lean screen
    // showed (active/awaiting/decision-count), presented as a badge here.
    bot_status_label_ = new QLabel(QStringLiteral("-"));
    bot_status_label_->setWordWrap(true);
    bot_status_label_->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:10px;font-weight:700;%4")
        .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), MF));
    layout->addWidget(bot_status_label_);

    auto* dec_hdr = new QLabel(tr("RECENT FORECAST DECISIONS"));
    dec_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::CYAN(), MF));
    layout->addWidget(dec_hdr);
    bot_decisions_table_ = new QTableWidget(0, 7, tab);
    bot_decisions_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Market"), tr("Side"), tr("Forecast P"), tr("Mkt Price"), tr("Edge"), tr("Gate")});
    bot_decisions_table_->setStyleSheet(table_style());
    bot_decisions_table_->verticalHeader()->setVisible(false);
    bot_decisions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bot_decisions_table_->setSelectionMode(QAbstractItemView::NoSelection);
    bot_decisions_table_->horizontalHeader()->setStretchLastSection(true);
    bot_decisions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(bot_decisions_table_, 1);

    auto* pos_hdr = new QLabel(tr("WEATHER PAPER POSITIONS"));
    pos_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::CYAN(), MF));
    layout->addWidget(pos_hdr);
    bot_positions_table_ = new QTableWidget(0, 8, tab);
    bot_positions_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Market"), tr("Side"), tr("Entry"), tr("Qty"), tr("State"), tr("P&L"), tr("Reason")});
    bot_positions_table_->setStyleSheet(table_style());
    bot_positions_table_->verticalHeader()->setVisible(false);
    bot_positions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bot_positions_table_->setSelectionMode(QAbstractItemView::NoSelection);
    bot_positions_table_->horizontalHeader()->setStretchLastSection(true);
    bot_positions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(bot_positions_table_, 1);

    // Task 7: confirmations for manually-placed paper orders (order-entry
    // mini form, right side of the BRACKETS tab). Session-local — these are
    // simulated fills, never a bot-strategy position, so they are kept
    // separate from bot_positions_table_'s sandbox_position rows above.
    auto* manual_hdr = new QLabel(tr("MANUAL PAPER ORDERS (this session)"));
    manual_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                  .arg(ui::colors::CYAN(), MF));
    layout->addWidget(manual_hdr);
    manual_orders_table_ = new QTableWidget(0, 6, tab);
    manual_orders_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Bracket"), tr("Side"), tr("Qty"), tr("Price"), tr("Status")});
    manual_orders_table_->setStyleSheet(table_style());
    manual_orders_table_->verticalHeader()->setVisible(false);
    manual_orders_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    manual_orders_table_->setSelectionMode(QAbstractItemView::NoSelection);
    manual_orders_table_->horizontalHeader()->setStretchLastSection(true);
    manual_orders_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(manual_orders_table_, 1);

    return tab;
}

pred::PredictionExchangeAdapter* WeatherScreen::adapter() const {
    return pred::PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
}

void WeatherScreen::wire_adapter() {
    auto* a = adapter();
    if (!a) {
        if (status_label_)
            status_label_->setText(tr("Adapter offline — Kalshi weather markets unavailable."));
        return;
    }
    connect(a, &pred::PredictionExchangeAdapter::events_ready, this, &WeatherScreen::populate_events);
    connect(a, &pred::PredictionExchangeAdapter::markets_ready, this, &WeatherScreen::populate_markets);
    connect(a, &pred::PredictionExchangeAdapter::order_book_ready, this, &WeatherScreen::render_order_book);
    connect(a, &pred::PredictionExchangeAdapter::recent_trades_ready, this, &WeatherScreen::render_trades);
    // Tier 1: real error state instead of a permanent "Loading…" — the
    // adapter is shared with KalshiScreen, so only react while our own
    // fetch is actually pending (mirrors KalshiScreen's own
    // market_list_fetch_in_flight_ guard on the same signal).
    connect(a, &pred::PredictionExchangeAdapter::error_occurred, this, &WeatherScreen::handle_fetch_error);
}

void WeatherScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
    // Bot panel refreshes every show (mirrors the retired lean predictions
    // screen's showEvent behavior) and keeps polling on bot_timer_ while
    // this screen stays visible.
    refresh_bot_panel();
    if (bot_timer_)
        bot_timer_->start();
}

void WeatherScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (bot_timer_)
        bot_timer_->stop();
}

void WeatherScreen::refresh() {
    // Tier 1: real load state machine. fetch_pending_ stays true until
    // populate_markets (success), handle_fetch_error (adapter error), or
    // handle_fetch_timeout (no reply at all within load_timeout_timer_'s
    // window) resolves it — so the browser can never get stuck showing
    // "Loading weather brackets…" forever.
    fetch_pending_ = true;
    set_status(tr("Loading weather brackets…"), ui::colors::TEXT_SECONDARY());
    if (load_timeout_timer_)
        load_timeout_timer_->start();

    // Reload the producer's evidence on the same cadence the market list
    // refreshes (WeatherScreen has no separate polling timer of its own —
    // refresh() IS that cadence, driven by first show / future manual
    // refresh) so the browser's Fcst-P/Edge columns and the forecast panel
    // never join stale forecasts against fresh markets.
    load_forecasts();
    auto* a = adapter();
    if (!a) {
        handle_fetch_error(QStringLiteral("Kalshi.fetch_category"), tr("Adapter offline."));
        return;
    }
    // Tier 1 root cause: Kalshi's real category for KXHIGH* series is
    // "Climate and Weather", not "Weather" — the latter resolves to zero
    // series via /series?category=…, which is why the browser used to sit
    // on "0 BRACKETS / Loading weather brackets…" forever. The category
    // fetch still returns every "Climate and Weather" series (not just the
    // bot's six cities); populate_markets() filters that down.
    a->list_events(QStringLiteral("Climate and Weather"), QStringLiteral("volume"), 200, 0);
}

void WeatherScreen::set_status(const QString& text, const QString& color) {
    if (!status_label_)
        return;
    status_label_->setText(text);
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                     .arg(color, MF));
}

void WeatherScreen::handle_fetch_error(const QString& context, const QString& message) {
    // The "kalshi" adapter is shared with KalshiScreen, which fires the same
    // fetch_category-family errors for its own (crypto/other) category
    // browsing. Only treat this as our error while our own fetch is
    // actually in flight, and only for the fetch_category family this
    // screen's refresh() drives — mirrors KalshiScreen's own
    // market_list_fetch_in_flight_ guard on this same broadcast signal.
    if (!fetch_pending_ || !context.startsWith(QStringLiteral("Kalshi.fetch_category")))
        return;
    fetch_pending_ = false;
    if (load_timeout_timer_)
        load_timeout_timer_->stop();

    if (bracket_table_)
        bracket_table_->setRowCount(0);
    row_market_index_.clear();
    markets_.clear();
    if (count_label_)
        count_label_->setText(QStringLiteral("0 BRACKETS"));
    set_status(tr("Fetch failed — %1. Refresh to retry.").arg(message.isEmpty() ? tr("no response") : message),
               ui::colors::NEGATIVE());
}

void WeatherScreen::handle_fetch_timeout() {
    if (!fetch_pending_)
        return;
    handle_fetch_error(QStringLiteral("Kalshi.fetch_category"), tr("request timed out"));
}

void WeatherScreen::load_forecasts() {
    namespace cli = openmarketterminal::cli;
    forecasts_ = WeatherEvidenceReader::load(cli::kalshi_evidence_path(QStringLiteral("kalshi-weather-plan.json")));
}

void WeatherScreen::populate_events(const QVector<pred::PredictionEvent>& events) {
    QVector<pred::PredictionMarket> flattened;
    for (const auto& event : events) {
        for (auto market : event.markets) {
            if (market.question.trimmed().isEmpty())
                market.question = event.title;
            market.extras.insert(QStringLiteral("event_title"), event.title);
            flattened.push_back(market);
        }
    }
    populate_markets(flattened);
}

void WeatherScreen::populate_markets(const QVector<pred::PredictionMarket>& markets) {
    if (!bracket_table_)
        return;

    // Tier 1: the "kalshi" adapter is shared with KalshiScreen, and both
    // screens connect to events_ready/markets_ready (render_order_book and
    // render_trades already guard against the same sharing on their own
    // signals). A crypto — or any other category — list KalshiScreen fetches
    // also lands here. Accept a payload only if it actually carries at
    // least one of the bot's six weather series, or it is a genuinely empty
    // reply to OUR OWN pending fetch (refresh() found zero brackets).
    // Anything else is someone else's fetch on the shared adapter and must
    // be ignored — otherwise a foreign payload would wipe an
    // already-loaded weather browser back to "No open weather brackets."
    const bool has_weather_market = std::any_of(markets.begin(), markets.end(), [](const auto& m) {
        return is_bot_weather_series(m.extras.value(QStringLiteral("series_ticker")).toString());
    });
    if (!has_weather_market && !(markets.isEmpty() && fetch_pending_))
        return;

    // Tier 1: a reply landed (success or genuinely-empty) — the fetch is no
    // longer pending, whatever the outcome below.
    fetch_pending_ = false;
    if (load_timeout_timer_)
        load_timeout_timer_->stop();

    // Tier 1: restrict the browser to the six weather series the bot trades
    // — "Climate and Weather" (the category refresh() now fetches) also
    // carries series the bot never touches, and those would otherwise drown
    // out the bot's own six cities.
    struct Row {
        pred::PredictionMarket market;
        bool has_forecast = false;
        BracketForecast fc;
        bool has_edge = false;
    };

    QHash<QString, QVector<Row>> groups;  // city -> rows
    QStringList group_order;              // first-seen order; re-sorted below
    int total_filtered = 0;
    for (const auto& market : markets) {
        const QString series_ticker = market.extras.value(QStringLiteral("series_ticker")).toString();
        if (!is_bot_weather_series(series_ticker))
            continue;
        ++total_filtered;

        Row row;
        row.market = market;
        // Joined by ticker (key.market_id) against the producer's evidence
        // (Task 4) — not every market has a forecast yet (evidence lags a
        // fresh market list by one producer cycle).
        const auto it = forecasts_.constFind(market.key.market_id);
        if (it != forecasts_.constEnd()) {
            row.has_forecast = true;
            row.fc = *it;
            row.has_edge = !std::isnan(row.fc.edge);
        }

        const QString city = city_for_series(series_ticker);
        if (!groups.contains(city))
            group_order.push_back(city);
        groups[city].push_back(row);
    }

    // Tier 3: sort rows within each city group by Edge descending (biggest
    // edge on top); rows without a computed edge yet fall back to volume.
    for (const auto& city : group_order) {
        auto& rows = groups[city];
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.has_edge != b.has_edge)
                return a.has_edge;
            if (a.has_edge && b.has_edge && a.fc.edge != b.fc.edge)
                return a.fc.edge > b.fc.edge;
            return a.market.volume > b.market.volume;
        });
    }

    // Order the city groups themselves by their best edge descending too, so
    // the single biggest edge in the whole browser lands in the first group.
    std::stable_sort(group_order.begin(), group_order.end(), [&](const QString& a, const QString& b) {
        const auto& rows_a = groups[a];
        const auto& rows_b = groups[b];
        const bool a_has = !rows_a.isEmpty() && rows_a.first().has_edge;
        const bool b_has = !rows_b.isEmpty() && rows_b.first().has_edge;
        if (a_has != b_has)
            return a_has;
        if (a_has && b_has && rows_a.first().fc.edge != rows_b.first().fc.edge)
            return rows_a.first().fc.edge > rows_b.first().fc.edge;
        return a < b;  // deterministic fallback
    });

    // Kept parallel to bracket_table_'s rows (via row_market_index_) so
    // select_bracket(row) can map a click straight back to the
    // PredictionMarket it needs to fetch.
    markets_.clear();
    row_market_index_.clear();
    bracket_table_->setRowCount(0);
    bracket_table_->clearSpans();

    int table_row = 0;
    for (const auto& city : group_order) {
        const auto& rows = groups[city];
        if (rows.isEmpty())
            continue;

        // Tier 3: group header — the city's forecast high, shown once, with
        // its brackets beneath it.
        bool header_has_forecast = false;
        double header_forecast = 0.0;
        for (const auto& r : rows) {
            if (r.has_forecast) {
                header_has_forecast = true;
                header_forecast = r.fc.forecast_high_f;
                break;
            }
        }
        const QString header_text = header_has_forecast
            ? tr("%1 — forecast %2°F").arg(city).arg(header_forecast, 0, 'f', 0)
            : tr("%1 — no forecast yet").arg(city);

        bracket_table_->insertRow(table_row);
        auto* header_item = cell(header_text, ui::colors::TEXT_PRIMARY());
        QFont header_font = header_item->font();
        header_font.setBold(true);
        header_item->setFont(header_font);
        header_item->setBackground(QColor(ui::colors::BG_RAISED()));
        header_item->setFlags(header_item->flags() & ~Qt::ItemIsSelectable);
        bracket_table_->setItem(table_row, 0, header_item);
        bracket_table_->setSpan(table_row, 0, 1, bracket_table_->columnCount());
        row_market_index_.push_back(-1);
        ++table_row;

        for (const auto& row : rows) {
            markets_.push_back(row.market);
            const int market_index = markets_.size() - 1;

            const QString bracket_label = row.market.question.isEmpty()
                                              ? row.market.extras.value(QStringLiteral("event_title")).toString()
                                              : row.market.question;
            const double mid = outcome_price(row.market, 0);

            auto set_numeric = [&](int col, const QString& text, const QString& color = {}) {
                auto* item = cell(text, color);
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                bracket_table_->setItem(table_row, col, item);
            };

            bracket_table_->insertRow(table_row);
            bracket_table_->setItem(table_row, 0, cell(city));
            bracket_table_->setItem(table_row, 1, cell(bracket_label));
            set_numeric(2, QString::asprintf("%.2f", mid));
            set_numeric(3, QString::asprintf("%.0f", row.market.volume));

            // Tier 3: color-coded Edge — green positive / red negative / dim
            // when no edge yet (never a bare "0.0" masquerading as no-edge).
            if (row.has_forecast) {
                set_numeric(4, QString::asprintf("%.1f%%", row.fc.forecast_p * 100.0));
                if (row.has_edge) {
                    const QString color = row.fc.edge > 0.0 ? ui::colors::POSITIVE()
                                         : row.fc.edge < 0.0 ? ui::colors::NEGATIVE()
                                                              : ui::colors::TEXT_TERTIARY();
                    set_numeric(5, QString::asprintf("%+.3f", row.fc.edge), color);
                } else {
                    set_numeric(5, QStringLiteral("—"), ui::colors::TEXT_TERTIARY());
                }
            } else {
                set_numeric(4, QStringLiteral("—"), ui::colors::TEXT_TERTIARY());
                set_numeric(5, QStringLiteral("—"), ui::colors::TEXT_TERTIARY());
            }

            row_market_index_.push_back(market_index);
            ++table_row;
        }
    }

    if (count_label_)
        count_label_->setText(QStringLiteral("%1 BRACKETS").arg(total_filtered));

    // Tier 1: a real empty/error state instead of a permanent spinner —
    // "no results" and "fetch failed" now read differently, and neither one
    // is ever left showing "Loading weather brackets…".
    if (total_filtered == 0)
        set_status(tr("No open weather brackets."), ui::colors::TEXT_SECONDARY());
    else
        set_status(tr("Weather brackets loaded — %1 evaluated across %2 cities.")
                       .arg(total_filtered).arg(group_order.size()),
                   ui::colors::TEXT_SECONDARY());

    // Keep the forecast panel in sync with whatever forecasts_ this reload
    // just joined against, even if the currently-selected bracket's row
    // didn't change — otherwise a future periodic refresh (Task 5+) would
    // leave the panel showing a stale forecast for the selection.
    update_forecast_panel();
}

void WeatherScreen::select_bracket(int row) {
    if (row < 0 || row >= row_market_index_.size())
        return;
    // Tier 3: bracket_table_'s rows interleave city group-header rows with
    // data rows; a header row maps to -1 and carries no market, so a click
    // on one is simply ignored (the previous selection, if any, stays put).
    const int market_index = row_market_index_[row];
    if (market_index < 0 || market_index >= markets_.size())
        return;

    selected_ = markets_[market_index];
    has_selection_ = true;

    if (detail_title_) {
        const QString label = selected_.question.isEmpty()
            ? selected_.extras.value(QStringLiteral("event_title")).toString()
            : selected_.question;
        detail_title_->setText(label.isEmpty() ? tr("Bracket selected") : label);
    }
    if (order_book_widget_)
        order_book_widget_->clear();
    if (trades_table_)
        trades_table_->setRowCount(0);
    update_forecast_panel();

    // Task 7: order-entry form tracks the selection — enable it and seed the
    // price default off the current side's mid so it's not left at 0.50 for
    // an obviously mispriced bracket.
    if (order_preview_button_)
        order_preview_button_->setEnabled(true);
    if (order_place_button_)
        order_place_button_->setEnabled(true);
    if (order_price_spin_ && order_side_combo_) {
        const int index = order_side_combo_->currentText() == QStringLiteral("NO") ? 1 : 0;
        const double mid = outcome_price(selected_, index);
        if (mid > 0.0)
            order_price_spin_->setValue(mid);
    }

    auto* a = adapter();
    if (!a)
        return;
    // Mirrors KalshiScreen::select_market's fetch-and-render flow: the
    // first outcome's asset id drives the order book, the market's key
    // drives the trade tape.
    if (!selected_.outcomes.isEmpty())
        a->fetch_order_book(selected_.outcomes.first().asset_id);
    a->fetch_recent_trades(selected_.key, 100);
}

void WeatherScreen::render_order_book(const pred::PredictionOrderBook& book) {
    if (!order_book_widget_ || !has_selection_)
        return;
    if (!selected_.key.asset_ids.contains(book.asset_id))
        return;

    QVector<QPair<double, double>> bids;
    bids.reserve(book.bids.size());
    for (const auto& level : book.bids)
        bids.append({level.price, level.size});

    QVector<QPair<double, double>> asks;
    asks.reserve(book.asks.size());
    for (const auto& level : book.asks)
        asks.append({level.price, level.size});

    double spread = 0.0;
    double spread_pct = 0.0;
    if (!bids.isEmpty() && !asks.isEmpty()) {
        const double best_bid = bids.first().first;
        const double best_ask = asks.first().first;
        spread = best_ask - best_bid;
        const double mid = (best_ask + best_bid) / 2.0;
        spread_pct = mid > 0.0 ? (spread / mid) * 100.0 : 0.0;
    }

    order_book_widget_->set_data(bids, asks, spread, spread_pct);
}

void WeatherScreen::render_trades(const QVector<pred::PredictionTrade>& trades) {
    if (!trades_table_ || !has_selection_)
        return;
    // recent_trades_ready is broadcast on the shared "kalshi" adapter, so
    // KalshiScreen's own fetch_recent_trades() calls land here too. Only
    // render a batch that actually belongs to the selected bracket's assets
    // (mirrors the asset_id guard in render_order_book above).
    if (!trades.isEmpty() && !selected_.key.asset_ids.contains(trades.first().asset_id))
        return;

    trades_table_->setRowCount(trades.size());
    for (int row = 0; row < trades.size(); ++row) {
        const auto& trade = trades[row];
        trades_table_->setItem(row, 0, cell(QDateTime::fromMSecsSinceEpoch(trade.ts_ms).toString(QStringLiteral("hh:mm:ss"))));
        trades_table_->setItem(row, 1, cell(trade.side));
        trades_table_->setItem(row, 2, cell(QString::asprintf("%.2f", trade.price)));
        trades_table_->setItem(row, 3, cell(QString::asprintf("%.2f", trade.size)));
    }
}

void WeatherScreen::update_forecast_panel() {
    if (!forecast_stats_label_ || !forecast_edge_label_ || !forecast_window_badge_ || !forecast_threshold_label_)
        return;
    auto* line = static_cast<ForecastNumberLine*>(forecast_line_widget_);

    if (!has_selection_) {
        forecast_stats_label_->setText(tr("Select a bracket for its forecast."));
        forecast_edge_label_->setText(QStringLiteral("EDGE —"));
        forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                                .arg(ui::colors::TEXT_TERTIARY(), MF));
        forecast_window_badge_->setText(QStringLiteral("—"));
        forecast_threshold_label_->setText(QString());
        if (line) line->clear_data();
        return;
    }

    const auto it = forecasts_.constFind(selected_.key.market_id);
    if (it == forecasts_.constEnd()) {
        forecast_stats_label_->setText(tr("No producer forecast for this bracket yet."));
        forecast_edge_label_->setText(QStringLiteral("EDGE —"));
        forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                                .arg(ui::colors::TEXT_TERTIARY(), MF));
        forecast_window_badge_->setText(QStringLiteral("—"));
        forecast_threshold_label_->setText(QString());
        if (line) line->clear_data();
        return;
    }

    const BracketForecast& fc = *it;
    const double market_mid = outcome_price(selected_, 0);
    forecast_stats_label_->setText(
        tr("Fcst High: %1°F   Fcst-P: %2%   Mkt Mid: %3")
            .arg(fc.forecast_high_f, 0, 'f', 1)
            .arg(fc.forecast_p * 100.0, 0, 'f', 1)
            .arg(market_mid, 0, 'f', 2));

    // Edge is producer-computed (never re-derived here — Global Constraints:
    // one forecast source of truth) and only meaningful when the bracket's
    // book was actually fetched; a NaN edge (out-of-window bracket) reads as
    // a neutral dash rather than a false "no edge" zero.
    if (std::isnan(fc.edge)) {
        forecast_edge_label_->setText(QStringLiteral("EDGE —"));
        forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                                .arg(ui::colors::TEXT_TERTIARY(), MF));
    } else {
        const QString color = fc.edge > 0.0 ? ui::colors::POSITIVE()
                             : fc.edge < 0.0 ? ui::colors::NEGATIVE()
                                              : ui::colors::TEXT_TERTIARY();
        forecast_edge_label_->setText(QStringLiteral("EDGE %1%2").arg(fc.edge >= 0.0 ? "+" : "")
                                          .arg(fc.edge, 0, 'f', 3));
        forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                                .arg(color, MF));
    }

    forecast_window_badge_->setText(fc.in_window ? tr("IN WINDOW") : tr("OUT OF WINDOW"));
    const QString badge_color = fc.in_window ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY();
    forecast_window_badge_->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %1;padding:2px 6px;font-size:9px;font-weight:800;%3")
                                              .arg(badge_color, ui::colors::BG_RAISED(), MF));

    // Small forecast-high-vs-threshold indicator: the strike (floor/cap) the
    // forecast has to clear, read from the market's own extras (the same
    // strike_type/floor_strike/cap_strike KalshiRestClient attaches), not
    // duplicated math — just a plain numeric comparison for display.
    const QString strike_type = selected_.extras.value(QStringLiteral("strike_type")).toString();
    const QVariant floor_v = selected_.extras.value(QStringLiteral("floor_strike"));
    const QVariant cap_v = selected_.extras.value(QStringLiteral("cap_strike"));
    QString threshold_text;
    bool has_floor = false, has_cap = false, has_threshold = false, meets = false;
    double floor_strike = 0.0, cap_strike = 0.0;
    if (strike_type == QStringLiteral("greater") && floor_v.isValid()) {
        floor_strike = floor_v.toDouble();
        has_floor = true;
        has_threshold = true;
        meets = fc.forecast_high_f >= floor_strike;
        threshold_text = tr("Fcst %1°F vs strike ≥%2°F → %3")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(floor_strike, 0, 'f', 1)
                             .arg(meets ? tr("ABOVE") : tr("BELOW"));
    } else if (strike_type == QStringLiteral("less") && cap_v.isValid()) {
        cap_strike = cap_v.toDouble();
        has_cap = true;
        has_threshold = true;
        meets = fc.forecast_high_f <= cap_strike;
        threshold_text = tr("Fcst %1°F vs strike ≤%2°F → %3")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(cap_strike, 0, 'f', 1)
                             .arg(meets ? tr("BELOW") : tr("ABOVE"));
    } else if (strike_type == QStringLiteral("between") && floor_v.isValid() && cap_v.isValid()) {
        floor_strike = floor_v.toDouble();
        cap_strike = cap_v.toDouble();
        has_floor = true;
        has_cap = true;
        has_threshold = true;
        meets = fc.forecast_high_f >= floor_strike && fc.forecast_high_f <= cap_strike;
        threshold_text = tr("Fcst %1°F vs range [%2°F, %3°F] → %4")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(floor_strike, 0, 'f', 1)
                             .arg(cap_strike, 0, 'f', 1)
                             .arg(meets ? tr("INSIDE") : tr("OUTSIDE"));
    }
    forecast_threshold_label_->setText(threshold_text);
    if (line)
        line->set_data(fc.forecast_high_f, has_floor, floor_strike, has_cap, cap_strike, has_threshold, meets);
}

void WeatherScreen::build_order_entry_panel(QVBoxLayout* into) {
    auto* order_hdr = new QLabel(tr("PAPER ORDER ENTRY (selected bracket)"));
    order_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                 .arg(ui::colors::CYAN(), MF));
    into->addWidget(order_hdr);

    auto* form_row = new QHBoxLayout;
    form_row->setSpacing(8);

    order_side_combo_ = new QComboBox(this);
    order_side_combo_->addItems({QStringLiteral("YES"), QStringLiteral("NO")});
    order_side_combo_->setToolTip(tr("Outcome to buy — YES or NO."));
    connect(order_side_combo_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        // Re-seed the price default off the newly selected outcome's mid so
        // switching YES/NO doesn't leave a stale price from the other side.
        if (!has_selection_ || !order_price_spin_)
            return;
        const int index = order_side_combo_->currentText() == QStringLiteral("NO") ? 1 : 0;
        const double mid = outcome_price(selected_, index);
        if (mid > 0.0)
            order_price_spin_->setValue(mid);
    });
    form_row->addWidget(order_side_combo_);

    order_price_spin_ = new QDoubleSpinBox(this);
    order_price_spin_->setRange(0.01, 0.99);
    order_price_spin_->setSingleStep(0.01);
    order_price_spin_->setDecimals(2);
    order_price_spin_->setValue(0.50);
    order_price_spin_->setPrefix(QStringLiteral("$"));
    order_price_spin_->setToolTip(tr("Limit price per contract (probability, 1-99c)."));
    form_row->addWidget(order_price_spin_);

    order_qty_spin_ = new QSpinBox(this);
    order_qty_spin_->setRange(1, 100);
    order_qty_spin_->setValue(5);  // small default paper size
    order_qty_spin_->setSuffix(tr(" ct"));
    order_qty_spin_->setToolTip(tr("Contracts (paper size)."));
    form_row->addWidget(order_qty_spin_);

    order_preview_button_ = new QPushButton(tr("PREVIEW"), this);
    order_preview_button_->setEnabled(false);
    connect(order_preview_button_, &QPushButton::clicked, this, &WeatherScreen::preview_paper_order);
    form_row->addWidget(order_preview_button_);

    order_place_button_ = new QPushButton(tr("PLACE PAPER ORDER"), this);
    order_place_button_->setEnabled(false);
    order_place_button_->setStyleSheet(QString("QPushButton{background:%1;color:%2;font-weight:800;padding:4px 10px;}")
                                           .arg(ui::colors::BG_RAISED(), ui::colors::AMBER()));
    connect(order_place_button_, &QPushButton::clicked, this, &WeatherScreen::place_paper_order);
    form_row->addWidget(order_place_button_);

    into->addLayout(form_row);

    order_confirm_label_ = new QLabel(tr("PAPER ONLY — no live or exchange order is ever submitted here."));
    order_confirm_label_->setWordWrap(true);
    order_confirm_label_->setStyleSheet(QString(
        "color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:10px;font-weight:700;%4")
        .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), MF));
    into->addWidget(order_confirm_label_);
}

void WeatherScreen::preview_paper_order() {
    if (!has_selection_) {
        QMessageBox::information(this, QStringLiteral("Weather"), tr("Select a bracket first."));
        return;
    }
    const QString side = order_side_combo_->currentText();
    const double p = order_price_spin_->value();
    const int n = order_qty_spin_->value();
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(selected_, p, n);
    const QString bracket_label = selected_.question.isEmpty()
        ? selected_.extras.value(QStringLiteral("event_title")).toString()
        : selected_.question;
    QMessageBox::information(this, tr("Paper order preview"),
        tr("%1 %2\n%3\n\n%4 contracts at %5\nNotional %6\nMaximum fee estimate %7\nMaximum loss %8\n"
           "Payout if correct %9\n\nThis review does not submit anything. PLACE PAPER ORDER simulates a "
           "local fill only — no live or exchange order is ever submitted for Weather.")
            .arg(QStringLiteral("BUY"), side, bracket_label)
            .arg(n).arg(order_probability(p)).arg(order_money(p * n)).arg(order_money(fee))
            .arg(order_money(p * n + fee)).arg(order_money(n)));
}

void WeatherScreen::place_paper_order() {
    if (!has_selection_) {
        QMessageBox::information(this, QStringLiteral("Weather"), tr("Select a bracket first."));
        return;
    }
    const QString side = order_side_combo_->currentText();
    const double p = order_price_spin_->value();
    const int n = order_qty_spin_->value();
    const QString bracket_label = selected_.question.isEmpty()
        ? selected_.extras.value(QStringLiteral("event_title")).toString()
        : selected_.question;

    const auto answer = QMessageBox::question(
        this, tr("Confirm paper order"),
        tr("%1 %2 contracts of %3 at %4 on:\n%5\n\nThis is a PAPER order. It will never be submitted to "
           "Kalshi or any exchange — it only simulates a local fill.")
            .arg(QStringLiteral("BUY")).arg(n).arg(side).arg(order_probability(p)).arg(bracket_label),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    // Build the OrderRequest for field parity with KalshiScreen's manual
    // ticket (asset_id/side/price/size/client_order_id) — but this request
    // is NEVER handed to PredictionExchangeAdapter::place_order(). Weather
    // order entry is paper-only per the Global Constraints; the fill below
    // is simulated entirely locally, so there is no code path here that can
    // reach the live/demo Kalshi API, regardless of has_credentials().
    const int outcome_index = side == QStringLiteral("NO") ? 1 : 0;
    pred::OrderRequest request;
    request.key = selected_.key;
    request.asset_id = selected_.outcomes.value(outcome_index).asset_id;
    request.side = QStringLiteral("BUY");
    request.order_type = QStringLiteral("LIMIT");
    request.price = p;
    request.size = n;
    request.client_order_id = QStringLiteral("PAPER-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.extras.insert(QStringLiteral("paper_only"), true);

    const QDateTime now = QDateTime::currentDateTime();
    if (order_confirm_label_) {
        order_confirm_label_->setText(
            tr("PAPER FILL — BUY %1 %2 @ %3 on %4 · order %5 · %6")
                .arg(n).arg(side).arg(order_probability(p)).arg(bracket_label, request.client_order_id,
                                                                  now.toString(QStringLiteral("hh:mm:ss"))));
        order_confirm_label_->setStyleSheet(QString(
            "color:%1;background:%2;border:1px solid %1;padding:4px 8px;font-size:10px;font-weight:700;%3")
            .arg(ui::colors::POSITIVE(), ui::colors::BG_RAISED(), MF));
    }

    if (manual_orders_table_) {
        manual_orders_table_->insertRow(0);
        manual_orders_table_->setItem(0, 0, cell(now.toString(QStringLiteral("hh:mm:ss"))));
        manual_orders_table_->setItem(0, 1, cell(bracket_label));
        manual_orders_table_->setItem(0, 2, cell(side, side == QStringLiteral("YES")
                                                             ? ui::colors::POSITIVE() : ui::colors::CYAN()));
        manual_orders_table_->setItem(0, 3, cell(QString::number(n)));
        manual_orders_table_->setItem(0, 4, cell(order_probability(p)));
        manual_orders_table_->setItem(0, 5, cell(QStringLiteral("PAPER FILLED"), ui::colors::POSITIVE()));
    }
}

void WeatherScreen::refresh_bot_panel() {
    populate_bot_summary();
    populate_bot_decisions();
    populate_bot_positions();
}

void WeatherScreen::populate_bot_summary() {
    const QString base =
        "FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "WHERE s.kind='kalshi_weather'";
    const int open = q_int("SELECT COUNT(*) " + base + " AND p.state IN ('open','pending_fill')");
    const int resolved = q_int("SELECT COUNT(*) " + base + " AND p.state='closed'");
    const int wins = q_int("SELECT COUNT(*) " + base + " AND p.state='closed' AND p.realized_pnl>0");
    const double pnl = q_double("SELECT COALESCE(SUM(p.realized_pnl),0) " + base);

    if (bot_open_count_)
        bot_open_count_->setText(QString::number(open));
    if (bot_resolved_count_)
        bot_resolved_count_->setText(QString::number(resolved));
    if (bot_win_count_)
        bot_win_count_->setText(resolved > 0 ? QStringLiteral("%1 (%2%)").arg(wins).arg(100 * wins / resolved)
                                             : QString::number(wins));
    if (bot_net_pnl_) {
        bot_net_pnl_->setText(QString::asprintf("%+.2f", pnl));
        bot_net_pnl_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                        .arg(pnl >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
    }
    populate_pnl_strip(open, resolved, wins, pnl);

    const bool strategy_active =
        q_int("SELECT COUNT(*) FROM sandbox_strategy WHERE kind='kalshi_weather' AND status='active'") > 0;
    const int recent_decisions =
        q_int("SELECT COUNT(*) FROM edge_decision_journal WHERE source='kalshi weather-plan'");
    if (bot_status_label_) {
        QString badge_color = ui::colors::TEXT_SECONDARY();
        if (!strategy_active) {
            bot_status_label_->setText(tr("Weather lane not seeded yet (run: sandbox seed)."));
            badge_color = ui::colors::TEXT_TERTIARY();
        } else if (recent_decisions == 0) {
            bot_status_label_->setText(tr("Weather lane active — awaiting the morning forecast window "
                                          "(producer runs during US active hours)."));
            badge_color = ui::colors::AMBER();
        } else {
            bot_status_label_->setText(tr("Weather lane active — %1 forecast decisions journaled.")
                                           .arg(recent_decisions));
            badge_color = ui::colors::POSITIVE();
        }
        bot_status_label_->setStyleSheet(QString(
            "color:%1;background:%2;border:1px solid %1;padding:4px 8px;font-size:10px;font-weight:700;%3")
            .arg(badge_color, ui::colors::BG_RAISED(), MF));
    }
}

void WeatherScreen::populate_pnl_strip(int open, int resolved, int wins, double pnl) {
    if (pnl_strip_positions_)
        pnl_strip_positions_->setText(QString::number(open));
    if (pnl_strip_settled_)
        pnl_strip_settled_->setText(QString::number(resolved));
    if (pnl_strip_net_) {
        pnl_strip_net_->setText(QString::asprintf("%+.2f", pnl));
        pnl_strip_net_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                          .arg(pnl >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
    }
    if (pnl_strip_winrate_)
        pnl_strip_winrate_->setText(resolved > 0 ? QStringLiteral("%1%").arg(100 * wins / resolved)
                                                  : QStringLiteral("—"));
}

void WeatherScreen::populate_bot_decisions() {
    if (!bot_decisions_table_)
        return;
    bot_decisions_table_->setRowCount(0);
    auto r = Database::instance().execute(
        "SELECT created_at, market_id, side, model_probability, market_probability, edge_after_cost, gate "
        "FROM edge_decision_journal WHERE source='kalshi weather-plan' "
        "ORDER BY created_at DESC LIMIT 60",
        {});
    if (!r.is_ok())
        return;
    auto& q = r.value();
    int row = 0;
    while (q.next()) {
        bot_decisions_table_->insertRow(row);
        const auto ts = QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong());
        const double edge = q.value(5).toDouble();
        const QString gate = q.value(6).toString();
        bot_decisions_table_->setItem(row, 0, cell(ts.toString(QStringLiteral("MM-dd HH:mm"))));
        bot_decisions_table_->setItem(row, 1, cell(q.value(1).toString()));
        bot_decisions_table_->setItem(row, 2, cell(q.value(2).toString().toUpper(),
                                               q.value(2).toString() == "yes" ? ui::colors::POSITIVE()
                                                                              : ui::colors::CYAN()));
        bot_decisions_table_->setItem(row, 3, cell(QString::asprintf("%.2f", q.value(3).toDouble())));
        bot_decisions_table_->setItem(row, 4, cell(QString::asprintf("%.2f", q.value(4).toDouble())));
        bot_decisions_table_->setItem(row, 5, cell(QString::asprintf("%+.3f", edge),
                                               edge >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
        bot_decisions_table_->setItem(row, 6, cell(gate.toUpper(),
                                               gate == "pass" ? ui::colors::AMBER() : ui::colors::TEXT_TERTIARY()));
        ++row;
    }
}

void WeatherScreen::populate_bot_positions() {
    if (!bot_positions_table_)
        return;
    bot_positions_table_->setRowCount(0);
    auto r = Database::instance().execute(
        "SELECT p.created_at, j.market_id, p.side, p.limit_price, p.qty, p.state, p.realized_pnl, p.close_reason "
        "FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "JOIN edge_decision_journal j ON j.id=p.decision_id "
        "WHERE s.kind='kalshi_weather' ORDER BY p.created_at DESC LIMIT 100",
        {});
    if (!r.is_ok())
        return;
    auto& q = r.value();
    int row = 0;
    while (q.next()) {
        bot_positions_table_->insertRow(row);
        const auto ts = QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong());
        const QString state = q.value(5).toString();
        const bool has_pnl = !q.value(6).isNull();
        const double pnl = q.value(6).toDouble();
        bot_positions_table_->setItem(row, 0, cell(ts.toString(QStringLiteral("MM-dd HH:mm"))));
        bot_positions_table_->setItem(row, 1, cell(q.value(1).toString()));
        bot_positions_table_->setItem(row, 2, cell(q.value(2).toString().toUpper()));
        bot_positions_table_->setItem(row, 3, cell(QString::asprintf("%.2f", q.value(3).toDouble())));
        bot_positions_table_->setItem(row, 4, cell(QString::asprintf("%.0f", q.value(4).toDouble())));
        bot_positions_table_->setItem(row, 5, cell(state.toUpper(),
                                               state == "open" ? ui::colors::AMBER() : ui::colors::TEXT_SECONDARY()));
        bot_positions_table_->setItem(row, 6, cell(has_pnl ? QString::asprintf("%+.2f", pnl) : QStringLiteral("-"),
                                               !has_pnl ? ui::colors::TEXT_TERTIARY()
                                                        : (pnl >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE())));
        bot_positions_table_->setItem(row, 7, cell(q.value(7).toString()));
        ++row;
    }
}

} // namespace openmarketterminal::screens
