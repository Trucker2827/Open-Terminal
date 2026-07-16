#pragma once
#include "core/result/Result.h"
#include "storage/repositories/AiFillRepository.h"

#include <QString>
#include <QVector>

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

/// A handler+symbol pair with its folded position.
struct HandlerPosition {
    QString handler;
    QString symbol;
    LedgerPosition position;
};

/// Append one paper fill: folds the handler+symbol's prior fills to the current position,
/// applies THIS fill for its realized PnL, and appends the row. Returns the appended row.
/// Rejects (Result::err, no row) when qty <= 0 or price <= 0. The only DB write in this service.
Result<AiFill> record_fill(const QString& handler, const QString& symbol, const QString& side,
                           double qty, double price, double fee, const QString& draft_id);

/// Fold a handler+symbol's fills (chronological) into its current position. Read-only.
LedgerPosition position_of(const QString& handler, const QString& symbol);

/// All (handler, symbol) with a non-flat net_qty, each folded. Empty handler = all handlers. Read-only.
QVector<HandlerPosition> positions_of(const QString& handler = {});

/// Aggregate signed net position for a symbol summed across ALL handlers. Read-only; 0 when flat/none/error.
double net_position_for_symbol(const QString& symbol);

/// Sum of realized_pnl across all fills (optionally for one handler). Read-only.
double realized_total(const QString& handler = {});

} // namespace ai_ledger
} // namespace openmarketterminal
