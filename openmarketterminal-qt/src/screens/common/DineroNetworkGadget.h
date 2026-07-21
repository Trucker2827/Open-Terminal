#pragma once

#include <QWidget>

class QLabel;
class QTimer;

namespace openmarketterminal::services::dinero {
class DineroRpcClient;
struct DineroStats;
}

namespace openmarketterminal::screens {

/// Compact, read-only Dinero telemetry for narrow trading-screen side rails.
/// External actions always use the operating system's configured browser.
class DineroNetworkGadget final : public QWidget {
    Q_OBJECT
  public:
    explicit DineroNetworkGadget(QWidget* parent = nullptr);

  private:
    void apply_stats(const services::dinero::DineroStats& stats);

    services::dinero::DineroRpcClient* client_ = nullptr;
    QTimer* timer_ = nullptr;
    QLabel* height_ = nullptr;
    QLabel* supply_ = nullptr;
    QLabel* block_time_ = nullptr;
};

} // namespace openmarketterminal::screens
