#include "services/ai_ledger/AiLedger.h"

#include "services/sandbox/PaperFillModel.h"

#include <QLatin1String>
#include <algorithm>
#include <cmath>

namespace openmarketterminal {
namespace ai_ledger {

FillDelta apply_fill(const LedgerPosition& current, const QString& side, double qty, double price, double fee) {
    const bool sell = (side == QLatin1String("sell") || side == QLatin1String("short"));
    const double signed_qty = sell ? -qty : qty;

    LedgerPosition p = current;
    double realized_this = 0.0;

    const double net = current.net_qty;
    const bool same_dir = (net == 0.0) || ((net > 0.0) == (signed_qty > 0.0));

    if (same_dir) {
        // Open or average-in. New avg is size-weighted; fee is a cost booked immediately.
        const double prev_abs = std::abs(net);
        const double add_abs = std::abs(signed_qty);
        const double total_abs = prev_abs + add_abs;
        p.avg_entry_price = total_abs > 0.0
            ? (current.avg_entry_price * prev_abs + price * add_abs) / total_abs
            : 0.0;
        p.net_qty = net + signed_qty;
        realized_this = -fee;
        p.realized_pnl = current.realized_pnl + realized_this;
    } else {
        // Opposite direction: close up to min(|net|, qty), realizing PnL on the closed portion.
        const double closing = std::min(std::abs(net), qty);
        const QString open_side = net > 0.0 ? QStringLiteral("buy") : QStringLiteral("sell");
        realized_this = services::sandbox::realized_pnl(open_side, current.avg_entry_price, price, closing, 0.0, fee);
        p.realized_pnl = current.realized_pnl + realized_this;

        const double leftover_open = qty - closing;  // > 0 only when the fill flips the position
        if (leftover_open > 0.0) {
            p.net_qty = (signed_qty > 0.0 ? 1.0 : -1.0) * leftover_open;
            p.avg_entry_price = price;
        } else {
            p.net_qty = net + signed_qty;  // moves toward zero
            if (p.net_qty == 0.0)
                p.avg_entry_price = 0.0;
            // avg_entry unchanged on a partial close
        }
    }
    return FillDelta{p, realized_this};
}

double unrealized_of(const LedgerPosition& p, double mark_price) {
    return (mark_price - p.avg_entry_price) * p.net_qty;
}

} // namespace ai_ledger
} // namespace openmarketterminal
