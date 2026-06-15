#pragma once
#include <QString>

namespace openmarketterminal::mcp::tools {

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

/// Deterministic, AI-uncontrolled daily-loss gate. today_loss = max(0, -realized)
/// (profits never expand headroom into negative); cap is the GUI-only
/// cli.risk.max_daily_loss (≤0 → finite default 5000, never "no cap").
/// Returns true iff (today_loss + prospective_max_loss) <= cap.
bool daily_loss_ok(double prospective_max_loss);

} // namespace openmarketterminal::mcp::tools
