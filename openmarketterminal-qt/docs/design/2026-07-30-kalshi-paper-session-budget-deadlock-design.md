# Kalshi paper bot — session-budget deadlock + orphaned-settlement leak — design

**Date:** 2026-07-30 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-paper-session-budget-deadlock` (off `main`)

## Why (the observed failure)

The paper bot stopped placing bids on **2026-07-28 ~13:29 UTC** and has passed on
100% of contracts since (96k consecutive `pass` decisions). Investigation of the
evidence bus (`kalshi-bot-decisions.jsonl`) established:

- It is **not** out of edge. Contracts clearing the `edge_threshold` of 0.10 have
  been continuously available — hundreds per hour on 2026-07-29/30 (e.g. 712 over
  the threshold in one hour). Of 8,513 post-Jul-28 decisions that cleared the edge
  **with a trusted signal**, 100% were refused.
- The refusals split: **5,681 `SESSION_BUDGET_BLOCKS_BID`** and **2,832 `ALREADY_HELD`**.
- `session_opened_usd` (the counter behind `SESSION_BUDGET_BLOCKS_BID`) is frozen at
  **119.83 for all 5,889 decisions across three days** — min = max, it never once
  decreases, including across the two paper settlements on Jul 28.

### Two independent defects

**1. Primary — the session budget bricks the perpetual paper loop.**
`decide()` (`KalshiBotDecision.cpp:399`) refuses a bid when
`session_opened_usd + all_in > session_budget_usd` (default `$120.00`).
`session_opened_usd` accumulates `all_in` on **every** bid
(`KalshiBotCommands.cpp:435`), is carried tick-to-tick across the loop's run
(`:1324`), and is **deliberately** carried across stop/resume — issue #125, "a
re-arm must not reset lifetime exposure" (`:325-327`). It resets **only** when the
loop process restarts (`:1305`). This is a **live bounded-run** safety: an armed
live run may commit at most `$X` of all-in before a human re-arms. Applied to the
**continuous paper-accumulation loop**, it is a lifetime cap: once cumulative paper
all-in reaches `$120`, the paper bot is bricked **forever** — and can never reach
the 300-settled gate it exists to build toward (which needs far more than `$120` of
cumulative bets). A deadlock.

**2. Secondary — orphaned paper positions leak open exposure.**
A paper position leaves the book only when `settle_paper` finds a settlement record
whose ticker matches, at the moment it runs (`KalshiBotDecision.cpp:494-512`;
`KalshiBotOrders.cpp:136-139` sets `order.settled` on the emitted
`kalshi_bot_paper_settlement` event). Paper positions settle off
`kalshi-settlements.jsonl`, which retains ~1 day (earliest retained row observed:
Jul 30 05:15). ~48 fills (80 filled − 32 settled) are on now-expired Jul 27-28
contracts whose settlement rows **rotated out before they were matched**. The code
path is explicit: *"No real settlement record → the position stays open"* (`:510`).
Those positions stay in `book.positions` **permanently**, inflating `at_risk_usd`
and the `held` set (the source of the 2,832 `ALREADY_HELD`). This is not the current
wall (`at_risk_usd` never hit `max_open_exposure_usd` — zero `EXPOSURE_CAP_BLOCKS_BID`
in the whole record), but left unfixed it will eventually cause an
`EXPOSURE_CAP_BLOCKS_BID` deadlock too, and it corrupts the open-position count.

### Not a bug (ruled out, to prevent a wrong "reconcile" task)

The EXPOSURE cockpit panel's `$0.00 of $120.00` binds to `worst_case_exposure_used`
/ `experiment_cap` of the **live armed session** (`BotCockpitPresentation.h:782-786`)
— genuinely `$0` because there are no live bids. That is a **different meter** from
the paper loop's `session_opened_usd`. Both are correct; there is no
divergence-accounting bug to reconcile.

## Goals & non-goals

**Goal:** the paper loop accumulates a track record without a lifetime cap, bounded
only by current open risk; expired paper positions that can never settle are retired
honestly. Live-mode bounded-run safety is preserved unchanged.

**Non-goals / NEVER:**
- **Never fabricate a settlement outcome.** A voided position records **no win and
  no loss** and is excluded from the scored track record — it is marked unresolved.
  This preserves the codebase's existing discipline (`KalshiBotDecision.cpp:511`,
  "no simulated outcome anywhere in this path") and the project rule that a fake
  winner is worse than no winner.
- **No look-ahead.** A position is voided only on the objective fact that its
  contract's close time is in the past by a margin — never by peeking at any later
  market state.
- **Do not weaken live-mode safety.** The live bounded-run session budget
  (issue #125) stays exactly as is; only the paper loop is exempted.
- No change to `edge_threshold`, `max_open_exposure_usd`, or the gate criteria.

## Fix 1 — exempt the paper loop from the lifetime session budget

`decide()` is shared by paper (`run_tick` → `:428`) and live
(`run_live_tick` → `:506/:522`), and `base_row` hardcodes `mode: "paper"`, so the
row's mode is not a usable discriminator. Introduce an explicit flag on `Config`:

```cpp
// KalshiBotDecision.h, in struct Config, beside session_budget_usd:
/// The session budget (issue #125) is a LIVE bounded-run safety: an armed run
/// may commit at most this all-in before a human re-arms, and it is not reset by
/// stop/resume. The perpetual paper loop has no arming boundary, so enforcing a
/// lifetime cap there is a deadlock — it bricks accumulation once cumulative
/// paper all-in reaches the cap, long before the 300-settled gate. The paper
/// loop sets this false; live leaves it true.
bool enforce_session_budget = true;
```

Guard the check in `decide()` (`:399`):

```cpp
if (config.enforce_session_budget &&
    session_opened_usd + all_in > config.session_budget_usd + 1e-9) {
    refuse_on_cap(kSessionBudgetBlocksBid, session_opened_usd, config.session_budget_usd);
    continue;
}
```

The **paper** caller (`run_tick`, where `config` is built) sets
`config.enforce_session_budget = false`. The **live** caller (`run_live_tick`)
leaves it `true`. `max_open_exposure_usd` (current open risk ≤ `$120` at once)
remains enforced in both — that is the real risk bound and the "paper measured under
the live fence" property is preserved for *open* exposure.

## Fix 2 — retire (void) provably-expired orphaned paper positions

In `settle_paper`, replace the unconditional *stay-open* branch (`:510-512`) with:
if no settlement record is found **and** the position's contract has provably closed
by a grace margin, emit a **void** row instead of leaving it open.

- **Expiry source (no look-ahead):** the contract close time is derived from the
  ticker via the existing Kalshi ticker parser (`KalshiRestClient` /
  `KalshiEvidenceEngine` parse the `KXBTCD-YYMMMDDHH…` / `KXBTC15M-YYMMMDDHHMM…`
  close encoding). A position is eligible to void only when
  `now_ms > contract_close_ms + kOrphanGraceMs`.
- **Grace margin `kOrphanGraceMs`:** long enough that a real settlement would have
  been recorded first (settlements for a just-closed contract appear within
  minutes), short enough to free exposure promptly. Proposed **6 hours**. A genuine
  settlement always wins the race: `settle_paper` matches real records first; the
  void branch is reached only when none exists **and** the contract closed > 6h ago.
- **The void row** (a new event, retired the same way settlements are — see below):

```
{ "event": "kalshi_bot_paper_void", "ts_ms": now, "mode": "paper",
  "position_id": <id>, "ticker": <ticker>, "side": <side>, "contracts": <n>,
  "stake_usd": <s>, "fee_usd": <f>,
  "resolution": "unresolved_expired",
  "reason": "contract closed with no settlement record in the retained window",
  "contract_close_ms": <parsed>, "won": null, "realized_pnl": 0.0 }
```

- **Retirement on replay:** `KalshiBotOrders::replay` sets `order.settled = true`
  on `kalshi_bot_paper_settlement` (`:136-139`). Extend it to treat
  `kalshi_bot_paper_void` the same way — the voided order leaves `book.positions`,
  releasing its `at_risk_usd` and clearing it from `held`.
- **Track-record exclusion:** the gate ledger and funnel count wins/losses and net
  P&L from `kalshi_bot_paper_settlement` rows only. `kalshi_bot_paper_void` carries
  `won: null` and `realized_pnl: 0.0` and MUST NOT be counted as a settled bid,
  a win, a loss, or into Brier. It is a retired-unresolved position, disclosed.

Because voided positions release open exposure, `at_risk_usd` stops leaking and the
paper loop stays legitimately under the `max_open_exposure_usd` fence over time.

## Interaction between the two fixes

Fix 1 is what unblocks bidding **now** (the current wall is `session_opened_usd`).
Fix 2 prevents a **future** `EXPOSURE_CAP_BLOCKS_BID` deadlock as orphans would
otherwise accumulate against `max_open_exposure_usd`, and removes the `ALREADY_HELD`
noise. Both are required for a durable fix; neither alone suffices.

## Testing (regression-first, must fail without the change)

Unit tests in `tst_kalshi_bot_decision.cpp` (and `tst_kalshi_bot_orders` /
`tst_command_dispatch` as the seams require):

1. **Fix 1 — paper accumulates past the budget.** With
   `enforce_session_budget = false`, a trusted report with edge-clearing contracts
   and `session_opened_usd` already at `session_budget_usd` still emits a `bid`
   (not `SESSION_BUDGET_BLOCKS_BID`). Neuter: with the flag `true` (live), the same
   inputs emit `SESSION_BUDGET_BLOCKS_BID` — proving the flag gates it and live is
   unchanged.
2. **Fix 1 — open-exposure fence still binds in paper.** With the budget exempted
   but `at_risk_usd + all_in > max_open_exposure_usd`, the bid is still refused with
   `EXPOSURE_CAP_BLOCKS_BID`. Proves paper is not made unbounded.
3. **Fix 2 — expired orphan voids.** An open position whose ticker close time is
   `> now - kOrphanGraceMs` in the past, with **no** settlement record, produces one
   `kalshi_bot_paper_void` (`won: null`, `realized_pnl: 0`), and after replay the
   position is gone from `book.positions` and its `at_risk_usd` is released.
4. **Fix 2 — real settlement still wins the race / no premature void.** (a) A
   position with a matching settlement record settles normally (win/loss P&L), never
   voids. (b) A position whose contract closed **less than** `kOrphanGraceMs` ago
   with no record stays open (not voided) — no premature retirement.
5. **Fix 2 — a void is never scored.** The gate ledger / funnel counting a fixture
   containing a `kalshi_bot_paper_void` row shows unchanged settled count, wins,
   losses, net P&L, and Brier — the void contributes nothing.

Every test must be shown to fail before the change and pass after.

## Rollout

1. Fix 1 (Config flag + `decide()` guard + `run_tick` sets it false) with tests 1-2.
2. Fix 2 (void branch in `settle_paper` + close-time parse + `replay` retirement +
   ledger/funnel exclusion) with tests 3-5.
3. Whole-branch review, then a rebuilt bot picks up both. Note: a bot **restart**
   alone (resetting `session_opened_usd` to 0) is a temporary stopgap that re-bricks
   in a few days without Fix 1.
