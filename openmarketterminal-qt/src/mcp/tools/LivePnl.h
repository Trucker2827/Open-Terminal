#pragma once
#include <QString>

#include "trading/TradingTypes.h" // trading::OrderSide

namespace openmarketterminal::mcp::tools {

/// The broker-status token that means "actually executed" for a LIVE fill
/// (ReconciledFill::status / OrderFlowTools' broker_status passthrough). A
/// broker adapter MUST normalize its reported status to exactly this token
/// for a real execution — StrategyRunner's live fill-lifecycle check
/// (broker_status == kLiveFilledStatus) is the sole consumer of this
/// contract; this constant exists so producer and consumer can never drift
/// on the literal string.
inline constexpr QLatin1String kLiveFilledStatus{"filled"};

/// Today's UTC day string (e.g. "2026-06-15") — the daily_pnl tally key.
QString today_utc();

/// Record a LIVE open/add at a known fill price into the realized-P&L ledger
/// (P&L-ONLY, NO cash side). Weighted-averages into any existing open position
/// for (account,venue,instrument). Mirrors PmPaperEngine::buy_to_open minus cash.
void record_open(const QString& account, const QString& venue, const QString& instrument,
                 double qty, double fill_price);

/// Record a LIVE close/reduce at a known fill price; returns the realized P&L
/// (fill_price - avg_cost) * closed_qty and adds it to today's daily_pnl tally.
/// An untracked close (no open position) returns 0 and contributes nothing —
/// conservative. Mirrors PmPaperEngine::sell_to_close minus cash.
double record_close(const QString& account, const QString& venue, const QString& instrument,
                    double qty, double fill_price);

/// Outcome of reconcile_and_record: the price/qty actually written to the ledger
/// and whether the broker's real fill price was used (vs the resolved fallback).
struct ReconciledFill {
    double price = 0;        // price actually recorded; 0 when nothing was recorded
    double qty = 0;          // broker-confirmed qty actually recorded; 0 when unfilled
    bool reconciled = false; // true iff the broker's actual avg fill price was used
    bool recorded = false;   // true iff a broker-confirmed non-zero fill changed the ledger
    QString status;          // broker's reported order status; empty if the order wasn't found
};

/// Best-effort broker-fill reconciliation for a LIVE fill, shared by both fill
/// sites (fast_submit + the equity submit live path). After place_order reports
/// success and the order_id is known, query the broker ONCE for that order's
/// actual cumulative fill quantity and average price. The ledger changes ONLY
/// when the broker confirms filled_qty > 0; an accepted/open/unfilled order is
/// exposure-neutral here. A confirmed fill with no average price uses
/// resolved_price for valuation, but never substitutes submitted qty for fill
/// qty. Routes confirmed fills to record_open (Buy) / record_close (Sell).
/// NEVER throws and NEVER blocks beyond the single get_orders call. Returns
/// what was actually recorded.
ReconciledFill reconcile_and_record(const QString& account, const QString& venue,
                                    const QString& instrument, trading::OrderSide side,
                                    double qty, double resolved_price, const QString& order_id);

/// Deterministic, AI-uncontrolled daily-loss gate. today_loss = max(0, -realized)
/// (profits never expand headroom into negative); cap is the GUI-only
/// cli.risk.max_daily_loss (≤0 → finite default 5000, never "no cap").
/// Returns true iff (today_loss + prospective_max_loss) <= cap.
bool daily_loss_ok(double prospective_max_loss);

} // namespace openmarketterminal::mcp::tools
