#pragma once
// DineroNetworkWidget — clean native Dinero tile (read-only, no trading).
//
// Live chain stats from the public Dinero RPC + a prominent GET DINERO button
// (opens dinerolabs.org) and an Open Explorer button. Deliberately NOT an
// embedded web view: a QWebEngineView inside a small dashboard tile paints over
// sibling controls on macOS and renders poorly. The full embedded explorer lives
// on the dedicated Dinero screen instead.

#include "screens/dashboard/widgets/BaseWidget.h"

#include <QJsonObject>

class QLabel;
class QPushButton;
class QTimer;

namespace openmarketterminal::services::dinero {
class DineroRpcClient;
struct DineroStats;
} // namespace openmarketterminal::services::dinero

namespace openmarketterminal::screens::widgets {

class DineroNetworkWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit DineroNetworkWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;

  private:
    void on_stats(const services::dinero::DineroStats& s);
    QWidget* make_stat(const QString& label, QLabel** value_out, int value_px);

    services::dinero::DineroRpcClient* client_ = nullptr;
    QTimer* timer_ = nullptr;
    QPushButton* get_btn_ = nullptr;
    QPushButton* exp_btn_ = nullptr;
    QLabel* height_ = nullptr;
    QLabel* supply_ = nullptr;
    QLabel* blocktime_ = nullptr;
    QLabel* reward_ = nullptr;
};

} // namespace openmarketterminal::screens::widgets
