#include "screens/weather/WeatherScreen.h"

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

    bracket_table_ = new QTableWidget(0, 4, this);
    bracket_table_->setHorizontalHeaderLabels({tr("City"), tr("Bracket"), tr("Mkt (mid)"), tr("Vol")});
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
    if (auto* a = adapter())
        a->list_events(QStringLiteral("Weather"), QStringLiteral("volume"), 200, 0);
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
        ++row;
    }

    if (count_label_)
        count_label_->setText(QStringLiteral("%1 BRACKETS").arg(sorted.size()));
    if (status_label_)
        status_label_->setText(sorted.isEmpty()
                                   ? tr("No weather brackets returned.")
                                   : tr("Weather brackets loaded — %1 evaluated.").arg(sorted.size()));
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

} // namespace openmarketterminal::screens
