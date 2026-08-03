#include "screens/weather/WeatherScreen.h"

#include "cli/ServeCommand.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPair>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::screens {

namespace pred = openmarketterminal::services::prediction;

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

/// City abbreviation from the series ticker prefix (e.g. "KXHIGHNY" for the
/// NYC daily-high-temperature series). Matches the city keys weather_producer
/// .py's WEATHER_CITIES table uses. Unknown prefixes fall back to the raw
/// series ticker so a new/unmapped city still shows something identifiable
/// rather than a blank cell.
QString city_for_series(const QString& series_ticker) {
    static const QHash<QString, QString> kKnownSeries = {
        {QStringLiteral("KXHIGHNY"), QStringLiteral("NYC")},
        {QStringLiteral("KXHIGHCHI"), QStringLiteral("CHI")},
        {QStringLiteral("KXHIGHDEN"), QStringLiteral("DEN")},
        {QStringLiteral("KXHIGHTSFO"), QStringLiteral("SFO")},
        {QStringLiteral("KXHIGHPHIL"), QStringLiteral("PHIL")},
        {QStringLiteral("KXHIGHTSEA"), QStringLiteral("SEA")},
    };
    return kKnownSeries.value(series_ticker, series_ticker);
}

} // namespace

WeatherScreen::WeatherScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    wire_adapter();
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
    root->addLayout(header_row);

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

    root->addLayout(content_row, 1);
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
}

void WeatherScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
}

void WeatherScreen::refresh() {
    if (status_label_)
        status_label_->setText(tr("Loading weather brackets…"));
    // Reload the producer's evidence on the same cadence the market list
    // refreshes (WeatherScreen has no separate polling timer of its own —
    // refresh() IS that cadence, driven by first show / future manual
    // refresh) so the browser's Fcst-P/Edge columns and the forecast panel
    // never join stale forecasts against fresh markets.
    load_forecasts();
    if (auto* a = adapter())
        a->list_events(QStringLiteral("Weather"), QStringLiteral("volume"), 200, 0);
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

    QVector<pred::PredictionMarket> sorted = markets;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.volume > right.volume;
    });
    // Kept parallel to bracket_table_'s rows so select_bracket(row) can map
    // a click straight back to the PredictionMarket it needs to fetch.
    markets_ = sorted;

    bracket_table_->setRowCount(0);
    int row = 0;
    for (const auto& market : sorted) {
        const QString series_ticker = market.extras.value(QStringLiteral("series_ticker")).toString();
        const QString city = city_for_series(series_ticker);
        const QString bracket_label = market.question.isEmpty()
                                          ? market.extras.value(QStringLiteral("event_title")).toString()
                                          : market.question;
        const double mid = outcome_price(market, 0);

        bracket_table_->insertRow(row);
        bracket_table_->setItem(row, 0, cell(city));
        bracket_table_->setItem(row, 1, cell(bracket_label));
        bracket_table_->setItem(row, 2, cell(QString::asprintf("%.2f", mid)));
        bracket_table_->setItem(row, 3, cell(QString::asprintf("%.0f", market.volume)));

        // Joined by ticker (key.market_id) against the producer's evidence
        // (Task 4) — not every market has a forecast yet (evidence lags a
        // fresh market list by one producer cycle), so a miss renders "—"
        // rather than a misleading blank/zero.
        const auto it = forecasts_.constFind(market.key.market_id);
        if (it != forecasts_.constEnd()) {
            bracket_table_->setItem(row, 4, cell(QString::asprintf("%.1f%%", it->forecast_p * 100.0)));
            if (std::isnan(it->edge)) {
                bracket_table_->setItem(row, 5, cell(QStringLiteral("—")));
            } else {
                const QString color =
                    it->edge > 0.0 ? ui::colors::POSITIVE() : (it->edge < 0.0 ? ui::colors::NEGATIVE() : QString());
                bracket_table_->setItem(row, 5, cell(QString::asprintf("%+.3f", it->edge), color));
            }
        } else {
            bracket_table_->setItem(row, 4, cell(QStringLiteral("—")));
            bracket_table_->setItem(row, 5, cell(QStringLiteral("—")));
        }
        ++row;
    }

    if (count_label_)
        count_label_->setText(QStringLiteral("%1 BRACKETS").arg(sorted.size()));
    if (status_label_)
        status_label_->setText(sorted.isEmpty()
                                   ? tr("No weather brackets returned.")
                                   : tr("Weather brackets loaded — %1 evaluated.").arg(sorted.size()));

    // Keep the forecast panel in sync with whatever forecasts_ this reload
    // just joined against, even if the currently-selected bracket's row
    // didn't change — otherwise a future periodic refresh (Task 5+) would
    // leave the panel showing a stale forecast for the selection.
    update_forecast_panel();
}

void WeatherScreen::select_bracket(int row) {
    if (row < 0 || row >= markets_.size())
        return;

    selected_ = markets_[row];
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

    if (!has_selection_) {
        forecast_stats_label_->setText(tr("Select a bracket for its forecast."));
        forecast_edge_label_->setText(QStringLiteral("EDGE —"));
        forecast_edge_label_->setStyleSheet(QString("color:%1;font-size:11px;font-weight:800;background:transparent;%2")
                                                .arg(ui::colors::TEXT_TERTIARY(), MF));
        forecast_window_badge_->setText(QStringLiteral("—"));
        forecast_threshold_label_->setText(QString());
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
    if (strike_type == QStringLiteral("greater") && floor_v.isValid()) {
        const double floor_strike = floor_v.toDouble();
        threshold_text = tr("Fcst %1°F vs strike ≥%2°F → %3")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(floor_strike, 0, 'f', 1)
                             .arg(fc.forecast_high_f >= floor_strike ? tr("ABOVE") : tr("BELOW"));
    } else if (strike_type == QStringLiteral("less") && cap_v.isValid()) {
        const double cap_strike = cap_v.toDouble();
        threshold_text = tr("Fcst %1°F vs strike ≤%2°F → %3")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(cap_strike, 0, 'f', 1)
                             .arg(fc.forecast_high_f <= cap_strike ? tr("BELOW") : tr("ABOVE"));
    } else if (strike_type == QStringLiteral("between") && floor_v.isValid() && cap_v.isValid()) {
        const double floor_strike = floor_v.toDouble();
        const double cap_strike = cap_v.toDouble();
        const bool inside = fc.forecast_high_f >= floor_strike && fc.forecast_high_f <= cap_strike;
        threshold_text = tr("Fcst %1°F vs range [%2°F, %3°F] → %4")
                             .arg(fc.forecast_high_f, 0, 'f', 1)
                             .arg(floor_strike, 0, 'f', 1)
                             .arg(cap_strike, 0, 'f', 1)
                             .arg(inside ? tr("INSIDE") : tr("OUTSIDE"));
    }
    forecast_threshold_label_->setText(threshold_text);
}

} // namespace openmarketterminal::screens
