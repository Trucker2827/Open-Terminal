#pragma once
// Crypto Predictions panel — shown in the Crypto Trading window when Coinbase is
// the selected source. Coinbase's prediction markets are operated through its
// Kalshi partnership (no separate Coinbase API), so this lists the crypto-
// related event contracts via the existing Kalshi prediction adapter.

#include "services/prediction/PredictionTypes.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens::crypto {

class CryptoPredictionsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoPredictionsPanel(QWidget* parent = nullptr);

    /// (Re)fetch crypto prediction markets from the Kalshi adapter.
    void refresh();

  private slots:
    void on_events_ready(const QVector<openmarketterminal::services::prediction::PredictionEvent>& events);
    void on_error(const QString& context, const QString& message);

  private:
    void retranslate();

    openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* list_ = nullptr;
};

} // namespace openmarketterminal::screens::crypto
