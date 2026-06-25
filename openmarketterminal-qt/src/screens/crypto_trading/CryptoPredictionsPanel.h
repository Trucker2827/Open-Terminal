#pragma once
// Crypto Predictions panel — shown in the Crypto Trading window. Coinbase's
// prediction markets are operated through its Kalshi partnership, so this lists
// the crypto event contracts via the Kalshi prediction adapter. A source toggle
// (Coinbase / Kalshi) re-labels the same data, and a cadence filter switches
// between 15-minute / hourly / daily crypto series.

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

    /// (Re)fetch crypto prediction events for the active source + cadence.
    void refresh();

  private slots:
    void on_events_ready(const QVector<openmarketterminal::services::prediction::PredictionEvent>& events);
    void on_error(const QString& context, const QString& message);

  private:
    void set_source(const QString& source);  // "Coinbase" | "Kalshi" (same data, relabel)
    void set_cadence(const QString& freq);    // "" = daily | "fifteen_min" | "hourly"
    void update_header();

    openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter_ = nullptr;

    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* src_coinbase_ = nullptr;
    QPushButton* src_kalshi_ = nullptr;
    QPushButton* cad_15m_ = nullptr;
    QPushButton* cad_1h_ = nullptr;
    QPushButton* cad_daily_ = nullptr;

    QString source_ = QStringLiteral("Coinbase");
    QString cadence_;  // empty = daily / base "Crypto"
};

} // namespace openmarketterminal::screens::crypto
