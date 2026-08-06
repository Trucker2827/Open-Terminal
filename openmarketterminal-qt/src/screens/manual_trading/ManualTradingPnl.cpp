#include "screens/manual_trading/ManualTradingPnl.h"

namespace openmarketterminal::screens {

double manual_position_pnl(double entry_price, double mark_price, int qty, double fee) {
    return (mark_price - entry_price) * static_cast<double>(qty) - fee;
}

} // namespace openmarketterminal::screens
