#pragma once
// PaperExecutor.h — one executor cycle for the Strategy Sandbox (Task 5).
//
// run_cycle drives every ACTIVE strategy (SandboxRegistry::list_strategies
// ("active")) through the paper-trading lifecycle for one profile:
//
//   1. Candidate selection & position opening, per strategy `kind` (driven by
//      params_json, not hardcoded per kind -- see the per-kind rules below).
//      New rows land in sandbox_position, deduped against decision_id.
//   2. pending_fill -> open|unfilled: resting limit orders (spot/scalp) are
//      checked against ticks since the position was created via
//      PaperFillModel::try_fill; positions whose entry window elapses
//      unfilled are marked 'unfilled'.
//   3. open -> closed: concrete (non-prediction, non-hypothetical) positions
//      are checked against ticks since they filled via
//      PaperFillModel::check_exit and closed on target/stop/expiry.
//
// Prediction (params `prediction`:true) and hypothetical (params
// `hypothetical`:true) books are OPENED directly into state 'open' here (no
// pending_fill phase -- there is no resting order, the "fill" is immediate
// at the journal row's reference price / market_probability) but are never
// advanced to 'closed' by run_cycle -- step 3's query is hard-filtered to
// `hypothetical = 0`, and predictions (side 'yes') aren't in its side
// allowlist either: that is the Outcome Resolver's job (Task 8,
// SandboxResolver.h), which run_cycle invokes at its own tail, and which is
// why sandbox_position.close_reason includes 'resolved' as a value run_cycle
// itself never writes. This applies to BOTH kinds equally: hypothetical
// (long_short) positions still get real tick-based target/stop/expiry exits
// -- just from the Resolver's own reuse of PaperFillModel::check_exit, not
// from this file's step 3.
//
// Candidate selection per kind:
//   - scalp: reimplements automation::latest_candidate's jsonl-scan contract
//     (see PaperExecutor.cpp) directly over <daemon_dir>/scalp_decisions.jsonl
//     (+ ".1"), because that helper lives in cli/ (not linked into
//     openterminal_core) -- see TickTail.h's layering note, which this file
//     follows by reusing the promoted TickTail::read_tail_with_prev.
//   - non-scalp, non-prediction, non-hypothetical (e.g. spot): edge_decision_
//     journal rows for the strategy's journal_source, gated by side='buy',
//     call='BUY CANDIDATE', gate='pass', max_age_sec, per-row horizon/
//     confidence thresholds, and a SQL-level anti-join against
//     sandbox_position.decision_id (the load-bearing "same candidate twice
//     -> skipped" dedup -- see PaperExecutor.cpp for the neuter-verified
//     query text; do not "harden" it with an extra pre-check, or a broken
//     anti-join stops failing loudly).
//   - prediction (params prediction:true, e.g. btc5m/kalshi): edge_decision_
//     journal rows for journal_source with gate='pass', opened at the row's
//     market_probability (side "yes"; a non-positive probability is a data
//     guard -- skipped, not fatal), also anti-joined against decision_id.
//   - hypothetical (params hypothetical:true, e.g. long_short): same journal
//     shape, opened at features_json.reference_price with side taken from
//     the row's own `side` column, target/stop from params target_bps/
//     stop_bps around that entry.
//
// Transactionality: candidate INSERTs are NOT soft-caught -- a duplicate
// decision_id that reaches INSERT (i.e. slipped past the anti-join) hits the
// sandbox_position.decision_id UNIQUE constraint and surfaces as a hard
// Result::err for the whole cycle. Position state TRANSITIONS (pending_fill
// -> open/unfilled, open -> closed), by contrast, use an optimistic
// UPDATE ... WHERE state='<expected>' guard: zero rows affected means a
// concurrent cycle already advanced that row, and is counted as `skipped`
// rather than treated as an error.

#include "core/result/Result.h"

#include <QString>
#include <QStringList>

namespace openmarketterminal::services::sandbox {

struct CycleReport {
    int opened = 0;
    int filled = 0;
    int unfilled = 0;
    int closed = 0;
    int skipped = 0;
    // Additive (Task 8): folded in from SandboxResolver::resolve_pending,
    // which run_cycle calls at its tail to settle prediction/hypothetical
    // books that steps 1-3 above deliberately never advance to 'closed'.
    int resolved = 0;
    int resolve_pending_count = 0;
    QStringList notes;
};

// One executor cycle for one profile. `daemon_dir` is the caller-resolved
// "<profile root>/daemon" directory (see AutomationState::state_dir) --
// resolving that path is a cli/ concern (profile_root_for), so it is passed
// in rather than recomputed here, keeping services/ free of cli/ includes.
// `now_ms` is the cycle's wall-clock time, threaded through explicitly so
// tests can drive expiry/unfilled transitions deterministically.
//
// Data-gap contract: a position that expires with NO pre-expiry tick to
// close against (PaperFillModel::check_exit's price-0 expiry sentinel) is
// closed with realized_pnl = NULL, close_reason = 'expiry', exit_fee = 0,
// and data_quality forced to 'degraded'. Closed rows with NULL realized_pnl
// are data-gap closes, not real round trips: the scorer (Task 9) MUST
// exclude them from resolved counts / pnl aggregates rather than coalescing
// NULL to 0.
//
// data_quality on newly opened positions is three-valued: 'ok' | 'degraded'
// | 'unknown'. 'unknown' means the candidate carried no freshness telemetry
// at all (neither freshest_age_ms nor live_sources -- e.g. btc5m/kalshi
// prediction journal rows); it is NOT evidence of degradation.
Result<CycleReport> run_cycle(const QString& profile, const QString& daemon_dir, qint64 now_ms);

} // namespace openmarketterminal::services::sandbox
