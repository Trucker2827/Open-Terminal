#pragma once
// DineroScreen — read-only "get familiar with Dinero" hub inside OpenMarket Terminal.
//
// Network overview (live stats via DineroRpcClient) + an honest "what is Dinero"
// blurb + links + an embedded block explorer (QtWebEngine when available, a
// browser-link fallback otherwise). No trading, no account, no payment.

#include <QWidget>

class QLabel;

namespace openmarketterminal::services::dinero {
class DineroRpcClient;
struct DineroStats;
} // namespace openmarketterminal::services::dinero

namespace openmarketterminal::screens {

class DineroScreen : public QWidget {
    Q_OBJECT
  public:
    explicit DineroScreen(QWidget* parent = nullptr);

  private:
    void on_stats(const services::dinero::DineroStats& s);
    QWidget* make_stat_tile(const QString& label, QLabel** value_out);

    services::dinero::DineroRpcClient* client_ = nullptr;
    QLabel* stat_height_ = nullptr;
    QLabel* stat_supply_ = nullptr;
    QLabel* stat_reward_ = nullptr;
    QLabel* stat_blocktime_ = nullptr;
};

} // namespace openmarketterminal::screens
