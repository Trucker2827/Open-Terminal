#pragma once
// SandboxEligibility.h — Live Eligibility Gate (Task 10): a pure,
// report-only evaluation of whether a sandbox strategy book has
// demonstrated enough evidence to be WORTH CONSIDERING for live promotion.
// This file computes a verdict and nothing else.
//
// REPORT-ONLY INVARIANT (binding): this header and SandboxEligibility.cpp
// MUST NOT #include anything from mcp/, trading/, or cli/. There is no code
// path from evaluate_eligibility (or the `sandbox eligibility` CLI command
// built on top of it) to any arm/order/setting -- it only ever reads
// already-computed leaderboard/registry data and returns a verdict string
// list for a human to read. Task 11's boundary (nm/objdump symbol) check
// enforces this for the whole sandbox; this file is the one under direct
// scrutiny since eligibility is the component closest to "turn it on."
//
// SPEC §4.7 bars (reviewed-code-change constants, not runtime knobs):
//   - active_days: UTC-calendar-day-floored days between the strategy's
//     FIRST sandbox_position.created_at and "now" (0 if the book has no
//     positions at all yet); must be >= kMinActiveDays.
//   - resolved: must be >= kMinResolvedSample. This constant is intentionally
//     NOT redeclared here -- it is SandboxScorer.h's own
//     `sandbox::kMinResolvedSample` (Task 9), reused as-is, so the
//     eligibility bar and the leaderboard's ranking bar can never drift out
//     of sync by editing one file and not the other.
//   - net_pnl: strictly `> 0` (a demonstrated edge, not merely "hasn't lost
//     money yet" -- exactly 0 is blocked).
//   - max_drawdown / gross_notional: (max_drawdown / gross_notional) must be
//     `<= kMaxDrawdownFrac`; exactly at the cap is ALLOWED. gross_notional
//     <= 0 (no resolved notional yet -- can only happen alongside
//     resolved == 0, which is already blocked by the resolved-sample bar)
//     is treated as a pass on this bar rather than a 0/0 division.
//   - degraded_count / total_positions: degraded_share =
//     degraded_count / max(1, total_positions) must be `< kMaxDegradedShare`;
//     exactly at the cap is BLOCKED. total_positions is ALL positions of
//     the book in EVERY state (open/pending_fill/closed/unfilled) --
//     matching LeaderboardRow::degraded, which is itself an all-state tally
//     (see SandboxScorer.h). 'unknown' data_quality positions are NOT
//     degraded (T5-review decision: no freshness telemetry at all is not
//     evidence of degradation) and so are correctly absent from the
//     numerator here (LeaderboardRow::degraded excludes them too) -- but
//     they DO still count in the denominator like any other position, same
//     as a book with many 'unknown' positions and zero 'degraded' ones
//     passes this bar cleanly.
//   - hypothetical books are ALWAYS blocked ("hypothetical instrument"),
//     unconditionally and before any other bar is even checked. Only
//     `params.hypothetical` triggers this -- `params.prediction` books
//     (e.g. btc5m/kalshi) are NOT hard-blocked here and can become eligible
//     if they clear every other bar below. Whether a prediction book (which
//     trades a real market, unlike a hypothetical one) should ever be
//     promotion-eligible is an open policy question left to a future review,
//     not something this file has decided.
//   - eligible iff the blockers list ends up empty; every failed bar adds
//     its own named blocker (blockers is not first-failure-wins -- a book
//     can be blocked for several reasons at once and the caller should see
//     all of them).

#include "services/sandbox/SandboxScorer.h" // reuse ::kMinResolvedSample only -- see above.

#include <QStringList>

namespace openmarketterminal::services::sandbox {

inline constexpr int    kMinActiveDays    = 28;
inline constexpr double kMaxDrawdownFrac  = 0.10;   // of cumulative gross notional.
inline constexpr double kMaxDegradedShare = 0.10;

struct EligibilityInput {
    int active_days = 0;
    int resolved = 0;
    int degraded = 0;
    int total_positions = 0; // ALL states -- see file header's denominator note.
    double net_pnl = 0;
    double max_drawdown = 0;
    double gross_notional = 0;
    bool hypothetical = false;
};

struct EligibilityVerdict {
    bool eligible = false;
    QStringList blockers;
};

EligibilityVerdict evaluate_eligibility(const EligibilityInput& in);

} // namespace openmarketterminal::services::sandbox
