#pragma once

#include "screens/common/IStatefulScreen.h"
#include "screens/weather/WeatherEvidenceReader.h"
#include "services/prediction/PredictionTypes.h"

#include <QHash>
#include <QShowEvent>
#include <QTableWidget>
#include <QWidget>

class QLabel;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens::crypto {
class CryptoOrderBook;
}

namespace openmarketterminal::screens {

/// Rich weather cockpit route (`weather`) — replaces the lean
/// WeatherPredictionsScreen as the route target. Task 2 delivers the shell
/// and a left-side bracket browser sourced straight from the
/// PredictionExchangeAdapter (same "kalshi" adapter instance KalshiScreen
/// uses), filtered server-side to the "Weather" category. Task 3 adds a
/// right-side detail pane: selecting a browser row fetches and renders
/// that bracket's order book (via the reused CryptoOrderBook widget — the
/// same generic bid/ask depth widget KalshiScreen embeds for its spot
/// reference DOM) and recent trades. Later tasks (forecast-vs-market, bot
/// panel, order entry) build out the remaining detail side.
class WeatherScreen final : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit WeatherScreen(QWidget* parent = nullptr);

    QVariantMap save_state() const override { return {}; }
    void restore_state(const QVariantMap&) override {}
    QString state_key() const override { return QStringLiteral("weather"); }
    int state_version() const override { return 1; }

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void build_ui();
    void wire_adapter();
    void refresh();
    services::prediction::PredictionExchangeAdapter* adapter() const;
    void populate_events(const QVector<services::prediction::PredictionEvent>& events);
    void populate_markets(const QVector<services::prediction::PredictionMarket>& markets);
    void select_bracket(int row);
    void render_order_book(const services::prediction::PredictionOrderBook& book);
    void render_trades(const QVector<services::prediction::PredictionTrade>& trades);
    void load_forecasts();
    void update_forecast_panel();

    QLabel* status_label_ = nullptr;
    QLabel* count_label_ = nullptr;
    QTableWidget* bracket_table_ = nullptr;
    bool first_show_ = true;

    // Task 3: bracket detail pane (order book + recent trades).
    QLabel* detail_title_ = nullptr;
    crypto::CryptoOrderBook* order_book_widget_ = nullptr;
    QTableWidget* trades_table_ = nullptr;
    QVector<services::prediction::PredictionMarket> markets_;
    services::prediction::PredictionMarket selected_;
    bool has_selection_ = false;

    // Task 4: forecast-vs-market panel, read from the producer's evidence
    // JSON (kalshi-weather-plan.json) via WeatherEvidenceReader and joined
    // to markets by ticker (PredictionMarket::key.market_id, which
    // KalshiRestClient populates from the raw Kalshi ticker — the same
    // string weather_producer.py's bracket_record() writes).
    QHash<QString, BracketForecast> forecasts_;
    QLabel* forecast_stats_label_ = nullptr;
    QLabel* forecast_edge_label_ = nullptr;
    QLabel* forecast_window_badge_ = nullptr;
    QLabel* forecast_threshold_label_ = nullptr;
};

} // namespace openmarketterminal::screens
