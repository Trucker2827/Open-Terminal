# Kalshi bot — cash-out / hold-cut engine — design

**Date:** 2026-08-01 · **Status:** design, pre-implementation ·
**Branch:** `finn/kalshi-holdcut-cashout` (off `main`)

## Why (supervision + operator's rules)
**TARGET ENGINE = `KalshiAutoEngine` (the `kalshi auto` bot that trades 15-min + live $2), NOT `KalshiBotDecision` (the separate calibrator paper bot).** Both are entry-only: `KalshiAutoEngine` reads existing positions ONLY to cap new entries (`existing_open_positions >= max_positions`) + reserve exit COST (`exit_cost_reserve`); it NEVER emits an exit/sell leg. So cash-out = **adding exit-leg generation to KalshiAutoEngine's portfolio planner** (a substantial new capability), then routing those legs through execute-next -> `sell_to_close`. The `KalshiBotDecision` `ALREADY_HELD` gap is real too but that's the calibrator bot, not the 15-min path. For
15-min BTC (a pure binary UP=yes / DOWN=no, decided in the final minutes) the
operator's rule is: the ONLY lever besides entry is **cash-out — sell-to-close
to CUT LOSSES or LOCK SURE WINS** before settlement. Without it the bot rides
every bet to all-or-nothing = the operator's #1 loss pattern (early edge that
reverses late). The execution primitive already exists (`OrderFlowTools`
`sell_to_close`, paper = pure DB writes); the decision layer never emits a cut.

## Goals & non-goals
**Goal:** turn `ALREADY_HELD` into a real **hold-or-cut** decision each tick over
filled positions, emitting a `CUT_POSITION` action that sells-to-close when it
economically beats holding — proven on the PAPER book first.

**Non-goals / NEVER:**
- **Paper-first. Do NOT touch the live rung yet.** `KalshiBotLive::live_intent`
  is deliberately buy-only/FAK/no-lifecycle. Live cash-out is a SEPARATE, later,
  deliberate promotion after paper proves the logic. This build routes cuts
  only through the paper/`sell_to_close` DB path.
- **Never churn.** A cut requires the trigger to persist ≥ N ticks (hysteresis),
  reusing the existing persistent-confirmation discipline.
- **Never cut when it loses money vs holding.** Cut only when
  `cash_out_proceeds − exit_cost > expected_hold_value`.
- **No new authority / same safety:** obeys the kill switch and caps exactly as
  entries do; a cut is journaled, never silent.

## The design
Replace the bare `ALREADY_HELD` skip (`KalshiBotDecision.cpp:240`) with an exit
evaluation over the held position, using data already in `decide()`
(`open_positions` rows carry side, entry price, contracts; the report carries
the current fair/market for the ticker):

1. **Current held-side value:** `mark = current market price for the side we
   hold` (from the report's book); `entry = position entry price`.
2. **Two cut triggers (either fires):**
   - **CUT-LOSS:** the model/market now puts the held side clearly *behind*
     — `held_side_prob < entry_prob − cut_loss_threshold` — i.e. the direction
     reversed. Cash out to stop the bleed before binary settlement.
   - **LOCK-WIN:** the held side is *solidly ahead late* — `held_side_prob >
     lock_win_threshold` AND `seconds_left < lock_win_window` (the decisive
     final minutes) — cash out near-full value to remove last-second-reversal
     risk. This is the 15-min "sure win" the operator named.
3. **Economics gate:** only cut when `mark_bid − fee > expected settlement value
   of holding` (cutting must beat riding). Hysteresis: trigger must hold ≥ N
   ticks.
4. **Emit** a `CUT_EDGE_REVERSED` / `LOCK_WIN` decision row (`action:"cut"`,
   `side:"sell"`, contracts = held size, price = current bid) → routed to
   `sell_to_close` (paper). Otherwise `HOLD_EDGE_INTACT` (was `ALREADY_HELD`).

New pure reason codes (unit-testable from `decide()` fixtures, no live I/O):
`HOLD_EDGE_INTACT`, `CUT_EDGE_REVERSED`, `LOCK_WIN`.

## Testing (pure `decide()` fixtures)
- **Cut a loser:** held YES, model/market for YES falls below entry − threshold
  for N ticks → `CUT_EDGE_REVERSED` + a sell row for the held size.
- **Lock a win:** held YES, prob > lock_win_threshold with < lock_win_window
  seconds left → `LOCK_WIN` + sell.
- **No cut on a one-tick blip:** trigger < N ticks → `HOLD_EDGE_INTACT`.
- **No cut when uneconomic:** trigger present but `bid − fee ≤ hold value` → hold.
- **Kill switch still governs:** a cut is refused when the switch is engaged.
- **Entry path unchanged (golden):** non-held tickers decide exactly as before.
- **15-min shape:** the lock-win window/threshold are tuned for the decisive
  final minutes of the 15-min race (config, not magic numbers).

## Rollout
1. This build — cut/lock decision + paper `sell_to_close` routing + tests.
2. Measure on the parallel-paper book (278+ closed positions): did cut/lock
   beat ride-to-settlement (realized P&L, avoided-loss / locked-win counts)?
3. ONLY if proven → deliberate follow-up: extend the live rung with a guarded
   sell-to-close path (its own design + review; the live rung was intentionally
   minimal). Never before.
