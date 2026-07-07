#pragma once
// SandboxResolver.h — Outcome Resolver for prediction and hypothetical books
// (Task 8): closes sandbox_position rows that PaperExecutor::run_cycle
// (Task 5) deliberately leaves 'open' forever -- prediction (side 'yes',
// hypothetical=0) and hypothetical (long_short, hypothetical=1) positions --
// against their real-world outcome.
//
// Journal `outcome` semantics (verified against the resolver call site, not
// assumed -- brief instruction): -1 unresolved/pending, 0 loss, 1 win. See
// src/cli/CommandDispatch.cpp:14387 ("UPDATE edge_decision_journal SET
// outcome=?, resolved_at=?, updated_at=? WHERE id=?"), whose bound value
// comes from edge_parse_outcome (CommandDispatch.cpp:16906-16917:
// "pending"/"unknown"/empty -> -1, win-family -> 1, loss-family -> 0) and
// matches edge_outcome_text (CommandDispatch.cpp:13068-13074) and the
// column's own DEFAULT -1 (v055_edge_decision_journal.cpp). This is exactly
// the brief's assumed -1/0/1 semantics -- no deviation to flag there.
//
// Prediction resolution (side 'yes', hypothetical=0): look up the position's
// decision_id in edge_decision_journal.outcome.
//   outcome == 1 -> payout 1.0/contract; realized_pnl = (1.0 - entry_price) *
//                   qty - entry_fee.
//   outcome == 0 -> payout 0;            realized_pnl = (0.0 - entry_price) *
//                   qty - entry_fee.
//   Neither settlement charges an exit fee (controller decision): a
//   prediction contract's settlement is not a trade -- there is no second
//   leg to charge a taker/maker fee on, unlike a concrete position's
//   target/stop/expiry close. close_reason is 'resolved' (not 'target'/
//   'stop'/'expiry'); the sandbox_fill row's kind is 'resolved', price =
//   payout.
//   outcome == -1, or no journal row found at all (defensive: treated the
//   same as unresolved, nothing to settle against) ->
//     now_ms >= expires_at + kResolveGraceMs -> close close_reason='expiry',
//       realized_pnl = NULL (data-gap contract -- see PaperExecutor.h's
//       run_cycle contract; Task 9's scorer MUST keep excluding NULL-pnl
//       rows the same way it already must for run_cycle's own data-gap
//       closes), fill note 'never resolved'.
//     otherwise -> left 'open', counted in ResolveReport::pending.
//
// Hypothetical resolution (long_short, hypothetical=1): CONTRARY TO the
// premise that PaperExecutor::advance_open_positions "already" tick-exits
// these off the normal cycle -- it does NOT. Its query is hard-filtered to
// `p.hypothetical = 0` (PaperExecutor.cpp, advance_open_positions' SELECT),
// and PaperExecutor.h's own run_cycle contract says prediction/hypothetical
// rows "are never advanced to 'closed' by run_cycle: that is the Outcome
// Resolver's job (Task 8)". The season-1 long_short seed
// (SandboxRegistry.cpp seed_default_strategies) trades BTC-USD -- the exact
// symbol scalp/spot already tick against in scalp_ticks.jsonl -- so real
// tick data for these positions exists and cannot be silently ignored:
// doing so would mean the long_short book never records a real win/loss,
// only ever NULL-pnl "never resolved" expiries. This resolver therefore
// reuses PaperFillModel::check_exit exactly like advance_open_positions does
// for concrete positions, which is why -- unlike an earlier 2-arg
// resolve_pending(profile, now_ms) sketch, which had zero existing callers
// to preserve compatibility for (grepped before widening) -- this function
// takes a ticks_path, mirroring run_cycle's own ticks_path plumbing exactly.
//   check_exit finds a real target/stop/expiry-with-a-tick-price exit ->
//     real close: realized_pnl via PaperFillModel::realized_pnl(entry=
//     limit_price, exit=exit.price, ...), exit_fee = fee_for(notional_usd,
//     taker_bps) from the strategy's params_json (same fee convention as
//     advance_open_positions), close_reason = exit.reason ('target'/'stop'/
//     'expiry'). hypothetical stays 1 on the row (set at open already).
//   check_exit's data-gap sentinel (reason 'expiry', price <= 0 -- no
//   pre-expiry tick at all) is NOT closed immediately at expires_at: ticks
//   are bounded at expires_at (PaperFillModel::check_exit's own contract),
//   so re-checking sooner cannot change today's verdict -- but a lagging
//   tick writer may still backfill pre-expiry ticks by the time the grace
//   window elapses, so this resolver waits. Only past expires_at +
//   kResolveGraceMs with STILL no pre-expiry tick -> close 'expiry',
//   realized_pnl NULL, fill note 'never resolved'; before that, left 'open'
//   and counted pending.
//   Not yet past its own expiry at all (check_exit not exited) -> left
//   alone entirely, not counted in either resolved or pending -- it is
//   still normally trading, not stuck awaiting resolution.
//
// Do NOT duplicate this file's target/stop tick-exit logic into
// advance_open_positions, or vice versa: the `hypothetical` flag is the
// single dispatch point between the two (0 -> PaperExecutor, 1 -> here).
//
// ResolveReport::resolved counts every position this call actually closed
// (win/loss settle, real tick exit, or grace-expired data gap alike);
// ::pending counts every examined-but-still-open prediction/hypothetical
// position at the end of the call (per the rules above -- not every 'open'
// row of that kind exists, only ones actually evaluated as awaiting
// resolution).

#include "core/result/Result.h"

#include <QString>

namespace openmarketterminal::services::sandbox {

struct ResolveReport {
    int resolved = 0;
    int pending = 0;
};

// 24h grace window past a position's expires_at before an unresolved
// prediction (journal outcome == -1) or a tick-data-gap hypothetical
// position is given up on as 'never resolved'. Exposed so tests can assert
// the exact boundary instead of poking a magic number.
inline constexpr qint64 kResolveGraceMs = 24LL * 3600 * 1000;

// One resolver pass for one profile: closes every currently-'open'
// prediction (side 'yes') and hypothetical (hypothetical=1) sandbox_position
// row it can settle, per the rules above. `ticks_path` is the caller-
// resolved "<daemon_dir>/scalp_ticks.jsonl" path (see PaperExecutor.h's
// daemon_dir note) -- passed straight through from run_cycle, which already
// has it. `now_ms` is threaded through explicitly, same reasoning as
// run_cycle, so tests can drive grace-period boundaries deterministically.
Result<ResolveReport> resolve_pending(const QString& profile, const QString& ticks_path, qint64 now_ms);

} // namespace openmarketterminal::services::sandbox
