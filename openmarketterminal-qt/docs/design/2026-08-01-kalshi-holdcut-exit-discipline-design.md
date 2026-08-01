# Kalshi bot — hold/cut exit discipline — design

**Date:** 2026-08-01 · **Status:** design, pre-implementation ·
**Branch:** `finn/kalshi-holdcut-exit-discipline`

## Why (found by supervision, from the operator's own loss pattern)

The biggest human losses came from a specific trap: **positive edge early in a
contract that reverses to negative in the final minutes** (spot near the strike
crosses back before expiry). Tracing `KalshiBotDecision::decide`, the bot has
the same hole: a filled position hits the `held.contains(ticker)` branch
(`KalshiBotDecision.cpp:240`), is stamped `ALREADY_HELD`, and is then **left
untouched until settlement** — no edge re-check, no cut. The bot has entry
discipline (time-conditioned edge gate, cancel-unfilled-on-vanished-edge) but
**no exit discipline.** It rides every filled position into whatever the last
minutes bring, exactly as a human holding through the decay would.

## Goal & non-goals

**Goal:** on each live tick, re-evaluate every FILLED position against the
current model, and CUT it (sell to exit) when its edge has reversed enough that
holding is worse than exiting after costs — turning `ALREADY_HELD`'s "do
nothing" into a real hold-or-cut decision.

**Non-goals / NEVER:**
- **Never churn on noise.** A cut requires the edge to have reversed past a
  hysteresis band, sustained (not a one-tick blip) — the same
  persistent-confirmation discipline entries already use.
- **Never cut when the exit costs more than it saves.** Selling pays the spread
  again; only cut when `expected loss avoided > exit cost`.
- **No new authority.** Exits obey the same kill switch, live-arm, and caps as
  entries; a cut is a normal order through the existing execution path.
- **No look-ahead / no fabricated exit price** — the cut decision reads only the
  current book + current model, same as entries.

## The design (turn ALREADY_HELD into hold-or-cut)

Replace the bare `ALREADY_HELD` skip with an exit evaluation:
1. For the held ticker, read the current model probability (calibrator/auto
   engine) and the current market book (the side we hold).
2. Compute the **held-side edge now**: `model_prob(held_side) - market_price(held_side)`.
   If it is still >= a hold floor, keep holding (`ALREADY_HELD`, unchanged).
3. If the edge has reversed past `-cut_threshold` AND persisted for
   >= N ticks (hysteresis) AND `exit_proceeds - exit_cost >
   expected_settlement_value` → emit a new `CUT_POSITION` decision that places a
   sell order to exit at the crossing price.
4. Everything else about the tick (entries, resting quotes, TTL) is unchanged.

New reason codes: `HOLD_EDGE_INTACT` (was ALREADY_HELD, edge still fine) and
`CUT_EDGE_REVERSED` (exiting). Both are pure classifications from the same pure
`decide()` inputs, so they are unit-testable with fixtures — no live I/O.

## Testing
- **Cut on sustained reversal:** a held YES position whose model prob falls
  below (market − cut_threshold) for N ticks → `CUT_EDGE_REVERSED` + a sell
  order for the held size.
- **No cut on a one-tick blip:** reversal for < N ticks → still `HOLD_EDGE_INTACT`.
- **No cut when exit is uneconomic:** reversal present but exit spread > expected
  loss avoided → hold.
- **Kill switch / caps still govern:** a cut is refused when the kill switch is
  engaged, same as an entry.
- **Entry path unchanged:** a golden check that non-held tickers decide exactly
  as before (this is purely additive to the held branch).

## Rollout
This is the roadmap's "hold/cut position management" phase, pulled forward
because supervision found it is the operator's #1 loss pattern. Ships behind the
unchanged kill switch + $2 micro caps; measured against settled outcomes in the
supervision loop (did cut positions avoid losses they would otherwise have taken?).
