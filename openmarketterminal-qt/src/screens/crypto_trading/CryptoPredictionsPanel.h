#pragma once
// Crypto Predictions panel — shown in the Crypto Trading window when Coinbase is
// the source. Coinbase's prediction markets are operated through Kalshi (no
// separate Coinbase API), so this lists the crypto event contracts via the
// Kalshi prediction adapter, with a cadence filter (15m / hourly / daily).
// Clicking a market opens the full Predictions screen to place a (real-money)
// Kalshi bet.

#include "services/prediction/PredictionTypes.h"

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens::crypto {

class CryptoPredictionsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoPredictionsPanel(QWidget* parent = nullptr);

    /// (Re)fetch crypto prediction events for the active cadence.
    void refresh();

  signals:
    /// User clicked a market and wants to bet — host opens the Predictions screen.
    void bet_requested();

  private slots:
    void on_events_ready(const QVector<openmarketterminal::services::prediction::PredictionEvent>& events);
    void on_error(const QString& context, const QString& message);

  private:
    void set_cadence(const QString& freq);  // "" = daily | "fifteen_min" | "hourly"

    openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter_ = nullptr;

    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* cad_15m_ = nullptr;
    QPushButton* cad_1h_ = nullptr;
    QPushButton* cad_daily_ = nullptr;

    QString cadence_;  // empty = daily / base "Crypto"
};

} // namespace openmarketterminal::screens::crypto
