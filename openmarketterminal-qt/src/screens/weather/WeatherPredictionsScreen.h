#pragma once

#include "screens/common/IStatefulScreen.h"

#include <QLabel>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

namespace openmarketterminal::screens {

/// Kalshi WEATHER predictions — a first-class, read-only view of the daily
/// city high-temp forecast-edge PAPER lane (edge_decision_journal
/// source='kalshi weather-plan', sandbox_strategy kind='kalshi_weather').
///
/// This is deliberately SEPARATE from the crypto KalshiScreen: it reads the
/// same generic sandbox tables filtered to the weather lane, so adding it
/// leaves the Bitcoin prediction cockpit completely untouched.
class WeatherPredictionsScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit WeatherPredictionsScreen(QWidget* parent = nullptr);

    QVariantMap save_state() const override { return {}; }
    void restore_state(const QVariantMap&) override {}
    QString state_key() const override { return QStringLiteral("weather"); }
    int state_version() const override { return 1; }

  public slots:
    void refresh();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void build_ui();
    void populate_summary();
    void populate_decisions();
    void populate_positions();

    QLabel* status_label_ = nullptr;
    QLabel* open_count_ = nullptr;
    QLabel* resolved_count_ = nullptr;
    QLabel* win_count_ = nullptr;
    QLabel* net_pnl_ = nullptr;
    QTableWidget* decisions_table_ = nullptr;
    QTableWidget* positions_table_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace openmarketterminal::screens
