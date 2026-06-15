#pragma once
// PmPaperEngine.h — Prediction-market PAPER fill engine (Phase B, Task 3).
//
// BUY-to-open / SELL-to-close paper fills over PmPaperRepository. These are
// PURE DB writes: they NEVER call the adapter's live place_order/cancel_order.
// Cash is debited on open and credited on close; cost-basis is averaged on a
// re-buy and reduced pro-rata on a partial sell. SELL-to-close ONLY — a SELL
// with no open position is rejected (Phase B never opens a short).

#include "storage/repositories/PmPaperRepository.h"

#include <QString>

namespace openmarketterminal::mcp::tools {

/// Result of a paper fill attempt. `ok=false` carries a human `reason` and the
/// repository is left UNMUTATED (no cash move, no position change).
struct PmFill {
    bool ok = false;
    QString reason;
    QString action;        // "buy_to_open" | "sell_to_close"
    double contracts = 0;  // contracts filled
    double fill_price = 0;
    double cash_after = 0;  // paper cash after the fill
};

/// BUY-to-open: debit cash by contracts*fill_price, then open a new position or
/// average into the existing OPEN position for (venue,asset_id). Rejects (no
/// mutation) when paper cash is insufficient.
PmFill buy_to_open(const QString& venue, const QString& market_id, const QString& asset_id,
                   const QString& outcome, const QString& category, double contracts,
                   double fill_price);

/// SELL-to-close: credit cash by contracts*fill_price and reduce the OPEN
/// position pro-rata (closing it when contracts reach ~0). Rejects when there is
/// no open position (short-open is not enabled) or when selling more than held.
PmFill sell_to_close(const QString& venue, const QString& asset_id, double contracts,
                     double fill_price);

/// Unrealized mark-to-market P&L for a position at `current_price`:
/// (current_price - avg_price) * contracts. No settlement.
double mark_to_market(const PmPosition& p, double current_price);

} // namespace openmarketterminal::mcp::tools
