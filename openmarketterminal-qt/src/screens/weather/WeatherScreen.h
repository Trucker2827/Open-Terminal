#pragma once

#include "screens/common/IStatefulScreen.h"
#include "services/prediction/PredictionTypes.h"

#include <QShowEvent>
#include <QTableWidget>
#include <QWidget>

class QLabel;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens {

/// Rich weather cockpit route (`weather`) — replaces the lean
/// WeatherPredictionsScreen as the route target. Task 2 delivers the shell
/// and a left-side bracket browser sourced straight from the
/// PredictionExchangeAdapter (same "kalshi" adapter instance KalshiScreen
/// uses), filtered server-side to the "Weather" category. Later tasks
/// (orderbook/trades, forecast-vs-market, bot panel, order entry) build out
/// the detail side of this shell — this task only wires the browser.
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

    QLabel* status_label_ = nullptr;
    QLabel* count_label_ = nullptr;
    QTableWidget* bracket_table_ = nullptr;
    bool first_show_ = true;
};

} // namespace openmarketterminal::screens
