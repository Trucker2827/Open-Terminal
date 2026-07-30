# Kalshi maker quote-lag engine — design

**Date:** 2026-07-30 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-maker-quote-lag` (off `main`)

## Why

The Kalshi edge autopsy (#169) found exactly one structural edge the market does not
already price: the book **lags spot**. After a ≥σ BTC move the Kalshi mid drifts a
further ~2¢ over ~15s. As a **taker** (crossing the spread) the fee eats it — which is
why every strategy-grid variant loses. As a **maker** (resting a quote, paying *no*
half-spread, only the fee), that same ~2¢ might clear. This is the one place a real
winner plausibly lives, and it has never been measured honestly, because a paper maker
engine that fakes fills produces confident garbage.

This engine measures it honestly. The whole feature turns on one thing: **a fill model
that cannot lie to us.**

## Goals & non-goals

**Goal:** measure whether resting a quote to capture the quote-lag is a real edge, with
a fill model bracketed between defensible optimistic/pessimistic bounds, and the same
statistical referee the strategy grid uses.

**Non-goals / NEVER:**
- **No live maker order placement.** This is research over recorded data only. Live
  maker execution is a far-future thing behind human arming + the existing security
  gates, out of scope here and not designed by this doc.
- **No fill-faking.** A quote fills only when the retained trade prints say a real
  trade went through its price on the taking side; queue position is bracketed, never
  assumed away.
- No autonomous path; no maker variants folded into the strategy grid yet (a later step
  once the fill model is trusted).

## Two components (bundled per "retain first, then sweep")

### 1. Retention extension (`kalshi_lag_series.py`)

A maker fill model needs **executed trade prints** (`kalshi-trade-events.jsonl`:
`yes_price_dollars`, `count_fp` size, `taker_side`/`taker_book_side`, `ts_ms`) and
**top-of-book sizes** (`yes_bid_size_fp`/`yes_ask_size_fp`) — the queue-position proxy.
Trade prints currently retain only **~2.8h** (size rotation), the binding constraint.

Add a **third retained stream** to the lag series: the trade prints
(`SOURCE_TRADES = "kalshi-trade-events.jsonl"`), time-bounded like the others, for the
same threshold + 15-minute families and expiry window. Confirm the retained ticker rows
already carry the `*_size_fp` fields (they are copied verbatim); if a filter drops them,
keep them. Trades are far sparser than quote changes, so the cost is small. This turns
2.8h into a real multi-day window over which a maker sweep is meaningful.

### 2. Maker quote-lag sweep (`scripts/research/maker_quote_lag.py`, read-only)

Reuses `kalshi_edge_common` (loaders, fees, outcomes, BRTI, vol) and
`q1_quote_lag.detect_events` (the σ-event detector on BRTI).

**Strategy under test.** On a ≥σ BTC move at time t, rest a maker quote on the
**lagging side** (the side the mid should drift toward) at the current touch (or one
tick inside), to capture the drift. Exit as a **taker** after a horizon
(5/15/30/60s) or hold to settlement. Net of the correct fees: maker fee on the resting
entry if it fills, taker fee on the exit.

**The bracketed fill model (the heart).** For each rested quote at price p placed at t,
compute fills two ways from the retained trade prints:
- **Optimistic (front of queue):** filled at the first subsequent trade that prints
  *through* p on the taking side.
- **Pessimistic (back of queue):** filled only after the cumulative taker volume that
  prints through p exceeds the size that was resting *ahead* of us at p when we joined
  (from the top-of-book size at t).
The realized edge is reported under **both** bounds. A quote that never fills under a
bound simply does not trade there — non-fill is a real outcome, counted, never a free win.

**Sweep parameters (pre-registered):** σ threshold × side × rest-offset (touch / +1
tick) × exit horizon × fill-bound.

## Statistical discipline (the strategy grid's referee, reused)

- Edge scored against **two baselines**: doing nothing, and the taker version of the
  same capture (so "maker beats taker" is explicit — the whole thesis).
- **Clustered effective-n** by triggering spot-move (events sharing one BTC move are not
  independent — the autopsy's core lesson).
- **Walk-forward** by event time.
- **Benjamini–Hochberg** across the parameter sweep (Bonferroni reported for reference).
- **Candidate surfacing** for forming near-misses (positive under the pessimistic bound
  but not yet significant), each with its shortfall.

**Certification rule:** an edge is only called real if it is positive under the
**pessimistic** fill bound **AND** clears BH **AND** persists out-of-sample **AND** has
adequate effective-n. An edge that clears only the optimistic bound is surfaced as a
flagged candidate, never sold. The honest headline may be "fee-eaten even as a maker" —
still a real answer, and the one the autopsy's prior predicts.

## Pre-registered parameters

Literals in the script, fixed before any run: σ thresholds `{2.0, 3.0}`; exit horizons
`{5, 15, 30, 60}s`; rest-offsets `{touch, +1 tick}`; event cooldown 300s; effective-n
survivor floor 30; BH α = 0.05. Maker/taker fees from `kalshi_edge_common`.

## Output artifacts

`maker-quote-lag.json` (stamped, versioned: provenance, method, and per-cell results
under both fill bounds with clustered stats + certification) and a dated
`docs/research/2026-…-kalshi-maker-quote-lag.md` report. Downstream, the strategy-grid
candidate view could surface a certified maker edge later — a follow-on, not this spec.

## Rollout

1. Retention extension (trade prints + sizes) + compactor tests.
2. The bracketed fill model as a pure, unit-tested function (the heart — tested on
   synthetic trade streams for both bounds and the non-fill case).
3. The event→rest→exit sweep + fee accounting.
4. Statistical layer (two baselines, clustered effective-n, walk-forward, BH,
   certification) + evidence artifacts + a read-only smoke run.

## Amendments during implementation (2026-07-30)

Recorded so this spec matches the shipped engine (final whole-branch review: "coherent
& honest"):

- **`ahead_size` uses real top-of-book size.** The pessimistic bound reads the resting
  side's size at join (YES bid → `yes_bid_size`, NO bid → `yes_ask_size`), via a
  backward-only `SizeBook` over the ticker feed — the sizes `q1.QuoteBook` drops.
  Without this the bracket is degenerate (pessimistic == optimistic); it is
  neuter-checked in the tests. Inside-spread rests (`+1¢` offset) have `ahead_size = 0`
  by construction (nothing rests ahead of a price between the touch), tagged
  `empty_level_inside_spread` — a logical necessity, not the old bug.
- **Certification is adversely-selected, not survivorship-inflated.** A record fills
  pessimistically only after *more* size trades through the level, which correlates
  with price moving against the resting bid — so conditioning on the pessimistic fill
  cannot manufacture a fake positive. This is *why* the pessimistic bound is the honest
  certifier.
- **Fees are symmetric (conservative).** Same `fee_per_contract` on both legs, no maker
  rebate modeled; if Kalshi rebates makers, the engine *understates* the edge.
- **Deferred to a follow-on (design mentioned, not in the task briefs; per-cell fields
  already support them):** Bonferroni-for-reference alongside BH; an explicit
  optimistic-only *candidate* view for near-misses; and the dated
  `docs/research/…-maker-quote-lag.md` human report (the engine emits stdout +
  `maker-quote-lag.json` today).
- **Current data reality:** retained trade prints are ~2.8h and (until the threshold
  families trade) KXBTC15M-heavy, so the honest headline is "no cell certifies /
  insufficient sample" — the engine degrades honestly rather than fabricating an edge.
  The verdict waits for accrual, exactly as "retain first, then sweep" intended.
