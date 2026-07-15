#pragma once
#include <QString>

namespace openmarketterminal {
namespace ai_ledger {

/// A handler+symbol's current derived state (folded from its fills).
struct LedgerPosition {
    double net_qty = 0.0;          ///< signed: + long, - short.
    double avg_entry_price = 0.0;  ///< avg entry of the open side (0 when flat).
    double realized_pnl = 0.0;     ///< cumulative realized (incl. fees) over all fills.
};

/// Result of applying one fill: the new position and the PnL realized by THIS fill.
struct FillDelta {
    LedgerPosition position;
    double realized_pnl_this_fill = 0.0;
};

/// Pure signed-position accounting for one fill.
/// side "buy"/"sell" (a "short" entry is treated as a sell). qty > 0, price, fee >= 0.
/// Same direction (or flat) → open/average-in (fee booked as a cost, no trade PnL).
/// Opposite direction → close min(|net|, qty) realizing via services::sandbox::realized_pnl;
/// any remainder flips the position (new avg_entry = fill price).
FillDelta apply_fill(const LedgerPosition& current, const QString& side, double qty, double price, double fee);

/// Mark-to-market of the open position: (mark - avg_entry) * net_qty (sign handles long/short).
double unrealized_of(const LedgerPosition& p, double mark_price);

} // namespace ai_ledger
} // namespace openmarketterminal
