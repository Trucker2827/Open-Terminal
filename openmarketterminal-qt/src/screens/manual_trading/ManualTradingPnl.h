#pragma once

// Pure, widget-free P&L core for the Manual Trading (paper) screen. Kept in
// its own translation unit (no Qt Widgets, no adapter) so the teaching
// invariant — opposite YES/NO across the two paper accounts on the SAME order
// book nets to roughly minus fees — can be unit-tested against real code
// (tests/manual_trading/tst_manual_trading_pnl.cpp) without linking the whole
// widget graph.

namespace openmarketterminal::screens {

/// Mark-to-mid net P&L for one paper position, in dollars.
///
/// Prices are probabilities in 0..1, i.e. dollars per $1 Kalshi contract.
/// `entry_price` is what was paid per contract, `mark_price` the current mid
/// for the SAME outcome, `qty` the number of contracts, `fee` the sunk taker
/// fee already paid at fill. Returns (mark - entry) * qty - fee.
double manual_position_pnl(double entry_price, double mark_price, int qty, double fee);

} // namespace openmarketterminal::screens
