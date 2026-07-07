#pragma once
// SandboxScorer.h — Scorer + Leaderboard for the Strategy Sandbox (Task 9):
// turns sandbox_position rows (opened by PaperExecutor, Task 5; settled by
// SandboxResolver, Task 8) into sandbox_score rows (migration v056) and a
// season-to-date leaderboard rollup.
//
// Binding data-quality contracts (carried over from PaperExecutor.h /
// SandboxResolver.h — violating these is an automatic reject):
//   1. A closed row with NULL realized_pnl is a data-gap close, not a real
//      round trip: resolved_count, net_pnl, hit_rate, avg_win/avg_loss, and
//      max_drawdown MUST exclude it entirely. NEVER coalesce NULL to 0 --
//      that would silently read as "closed flat."
//   2. degraded_count counts data_quality='degraded' positions only;
//      'unknown' (no freshness telemetry at all -- not evidence of
//      degradation, see PaperExecutor.h) is tallied separately as
//      unknown_count and MUST NOT be folded into degraded_count.
//   3. Fees are already netted into realized_pnl -- do not double-subtract.
//      gross_notional = SUM(notional_usd) over RESOLVED positions only.
//   4. "Resolved" = state='closed' AND realized_pnl IS NOT NULL. Positions
//      that expire unfilled land in state='unfilled' (never state='closed'
//      -- verified against PaperExecutor.cpp's pending_fill->unfilled
//      transition), so an extra close_reason != 'unfilled' guard would be
//      redundant, not wrong; the state check alone is exact.
//
// Two independent aggregation surfaces, sharing one per-bucket aggregator
// (see SandboxScorer.cpp's aggregate_bucket) so their arithmetic can never
// drift apart:
//   - score_all writes one sandbox_score row per (strategy_id, UTC calendar
//     day of closed_at) -- a historical, day-bucketed ledger consumed by the
//     CLI's `sandbox book` view.
//   - leaderboard() aggregates directly over ALL of a strategy's
//     sandbox_position rows (all-time, no day bucketing) rather than
//     summing sandbox_score's per-day rows back together: open_count is the
//     SAME live snapshot repeated on every day row (summing would multiply
//     it by day count), and hit_rate/avg_win/avg_loss are not additive
//     across days without also storing per-day win/loss counts, which the
//     schema does not have. Recomputing straight from sandbox_position sidesteps
//     both traps and guarantees leaderboard() always reflects the current
//     table, not whatever score_all last wrote.
//
// data_quality tallies (degraded_count / unknown_count) are deliberately
// NOT resolved-scoped like the pnl aggregates: they report on data quality
// across every position that reached a terminal, day-attributable state
// (closed or unfilled -- i.e. has closed_at set) for score_all's per-day
// rows, and across EVERY position regardless of state for leaderboard()'s
// all-time rollup (LeaderboardRow::degraded / ::unknown_count), so a
// currently-open position with degraded/unknown freshness still shows up in
// the book-health signal instead of waiting for it to close.
// data_gap_count (leaderboard-level only, no schema column) is narrower and
// explicit: COUNT positions with state='closed' AND realized_pnl IS NULL --
// contract 1's excluded rows, counted rather than discarded.

#include "core/result/Result.h"

#include <QList>
#include <QString>

namespace openmarketterminal::services::sandbox {

// Minimum resolved-round-trip count before a book's season-to-date stats
// are trusted enough to rank on the leaderboard. Shared by the CLI's
// display-bucketing logic and tests so neither duplicates the boundary.
inline constexpr int kMinResolvedSample = 30;

// Recomputes sandbox_score rows for every sandbox_strategy row (all
// statuses -- a paused/retired book's history is still worth scoring), for
// every UTC calendar day that has ANY sandbox_position activity for that
// strategy (a row with closed_at set -- i.e. state 'closed' or 'unfilled'),
// PLUS unconditionally "yesterday" and "today" (UTC, relative to now_ms)
// even with zero activity, so a freshly-seeded but still-idle strategy gets
// score rows instead of `sandbox book` finding nothing. Idempotent upsert
// (INSERT OR REPLACE keyed on the table's own (strategy_id, score_date)
// PRIMARY KEY): calling score_all twice back-to-back with no new activity
// between calls writes byte-identical rows both times.
//
// Per (strategy_id, score_date) bucket, over positions whose closed_at
// falls on that UTC day:
//   resolved_count / net_pnl / hit_rate / avg_win / avg_loss / gross_notional
//     -- see contracts 1/3/4 above.
//   unfilled_count = COUNT state='unfilled' that day.
//   degraded_count = COUNT data_quality='degraded' that day (contract 2;
//     spans both resolved and data-gap-closed rows -- see file header).
//   open_count = COUNT state IN ('open','pending_fill') for the strategy,
//     AS OF now_ms -- a live snapshot, not day-scoped (there is only one
//     "right now"), written identically onto every day row this call
//     touches for that strategy.
//   max_drawdown = peak-to-trough of the strategy's FULL cumulative
//     realized-pnl curve (every resolved position ever, ordered by
//     closed_at) -- not a per-day figure. Recomputed every call, but written
//     only onto the LATEST score_date row this call touches for that
//     strategy (every earlier day row this call writes gets 0, the
//     schema's own default -- those rows are a historical ledger entry, not
//     a retroactively-rewritten drawdown curve). Because "today" is always
//     among the touched days and closed_at can never exceed now_ms, the
//     latest touched day is always the same as the latest day with any
//     activity, so this is unambiguous.
Result<void> score_all(const QString& profile, qint64 now_ms);

struct LeaderboardRow {
    QString strategy_id, kind, status;
    int resolved = 0;
    double net_pnl = 0, hit_rate = 0, max_drawdown = 0, gross_notional = 0;
    int degraded = 0;
    bool hypothetical = false;
    // Leaderboard-level only -- sandbox_score has no columns for these per
    // this task's binding contract; computed live off sandbox_position, not
    // summed from sandbox_score rows. See file header.
    int unknown_count = 0;
    int data_gap_count = 0;
};

// Season-to-date rollup: one row per sandbox_strategy (all statuses),
// aggregated directly over that strategy's ENTIRE sandbox_position history
// (see file header for why this does not read sandbox_score back). kind /
// status come from sandbox_strategy; hypothetical is read from the
// strategy's own params_json (`hypothetical: true`, the exact key
// PaperExecutor.cpp's run_cycle branches on), not inferred per-position.
Result<QList<LeaderboardRow>> leaderboard(const QString& profile);

} // namespace openmarketterminal::services::sandbox
