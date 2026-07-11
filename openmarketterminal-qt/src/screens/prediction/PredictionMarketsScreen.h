#pragma once

#include "screens/common/IStatefulScreen.h"

#include <QWidget>

class QStackedWidget;

namespace openmarketterminal::screens {
class PolymarketScreen;
namespace kalshi { class KalshiScreen; }

class PredictionMarketsScreen final : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit PredictionMarketsScreen(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QVariantMap save_state() const override;
    void restore_state(const QVariantMap& state) override;
    QString state_key() const override { return QStringLiteral("prediction_markets"); }
    int state_version() const override { return 1; }

  private:
    void show_venue(const QString& venue);

    QStackedWidget* stack_ = nullptr;
    PolymarketScreen* polymarket_ = nullptr;
    kalshi::KalshiScreen* kalshi_ = nullptr;
    QString venue_ = QStringLiteral("kalshi");
};
} // namespace openmarketterminal::screens
