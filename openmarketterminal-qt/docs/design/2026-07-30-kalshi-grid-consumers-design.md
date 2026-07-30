# Kalshi strategy-grid consumers (CLI + bot line) — design

**Date:** 2026-07-30 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-grid-consumers` (off `finn/kalshi-strategy-grid`)

## Why

The strategy-grid engine writes `kalshi-strategy-grid-latest.json` but nothing reads
it yet. This makes the grid's verdict visible where decisions happen — a read-only CLI
verb and one advisory line in the bot cockpit — so the operator, the bot, the CLI, and
any agent can *see* what the grid measured. Per the operator's directive to "give us a
chance to have winners," the surfacing shows not only confirmed survivors but the top
**candidates** (near-misses) with the honest reason each falls short — so a *forming*
edge is visible as data accrues, instead of a binary "no edge."

**The discipline is non-negotiable.** This layer never loosens the survivor bar,
never recomputes, never gates or trades. A candidate is labelled a candidate, with its
distance from significance in plain sight; only a variant that beat *both* nulls
positively under correction + walk-forward is ever called `measured`. Making a real
edge visible is the goal; manufacturing a fake one is the failure this guards against.

## Goals & non-goals

**Goal:** read-only surfacing of `kalshi-strategy-grid-latest.json` in the CLI and the
bot cockpit, staleness-aware and fail-closed, with survivors AND top candidates each
carrying their humility metadata.

**Non-goals / never:** no recompute; no gate/trade coupling (surface-only, per the
operator's choice); no loosening of the survivor rule; no maker variants (that is the
**next, separate** feature — the maker quote-lag engine, queued). No autonomous path.

## Small upstream change: emit `candidates[]` in `-latest.json`

`strategy_grid.py`'s `latest_summary` currently emits only `survivors[]`. Add a
`candidates[]` block: the top **N = 8** NON-survivor variants ranked by `delta_vs_market`
descending, restricted to variants with `delta_vs_hold > 0 AND delta_vs_market > 0 AND
effective_n >= CANDIDATE_MIN_EFF_N (=10)` (a forming, positive edge on real sample —
not a random negative slice). Each candidate carries the same humility fields as a
survivor plus a `blocked_by` string naming why it is not `measured`:
`"not significant (BH); p=0.12"`, `"effective_n 18 < 30"`, or `"walk-forward sign
flip"`. When a variant fails more than one gate, `blocked_by` reports them in this
fixed precedence: **effective_n first, then BH significance, then walk-forward** (the
most fundamental shortfall first). When none qualify, `candidates: []`. This keeps the
compact consumer file self-contained (no consumer parses the full 468-variant grid).

## Architecture — one pure core, two thin consumers

Mirrors the repo's "pure presentation over evidence files" pattern (`BotCockpitPresentation`).

**1. Pure module `src/services/prediction/kalshi/KalshiStrategyGridView.{h,cpp}`** — the
JSON contract parsed and formatted in ONE tested place, no I/O of its own:
- `parse_latest(const QByteArray& json, qint64 now_ms) -> GridLatest` — struct
  `{ bool available; int schema_version; QString headline; qint64 age_ms; bool stale;
  QVector<Row> survivors; QVector<Row> candidates; }` where `Row = { variant_id, side,
  band, gate, exit, delta_vs_hold, delta_vs_market, effective_n, trust, blocked_by }`.
  **Fails closed** to `available=false` on missing/garbage/`schema_version` mismatch
  (the cockpit's issue-#145 discipline). Marks `stale=true` when `age_ms` exceeds a
  freshness bound `GRID_STALE_MS = 6h` (a literal constant beside
  `bot_cockpit_freeze_bound_ms`). Stale still shows the numbers, tagged `(STALE …)` —
  never hidden, never presented as current.
- `cockpit_line(const GridLatest&) -> QString` — ONE advisory line for the scene:
  `GRID: no measured edge` · `GRID ADVISORY: NO 10-25c + SL15 +2.1c vs mkt (n_eff 42)` ·
  `GRID: forming — NO 10-25c +0.8c vs mkt, p=0.11 (n_eff 34)` (top candidate when no
  survivor) · `GRID: UNAVAILABLE` · a `(STALE 3h)` suffix when stale.
- `cli_report(const GridLatest&, bool as_json) -> QString` — headline + survivor rows +
  a "closest candidates" section (each with `blocked_by`), or the raw JSON when `--json`.

**2. CLI verb** — `kalshi grid` (alias `strategy-grid`), a small command file registered
in `CommandDispatch.cpp` mirroring an existing read-only verb (e.g. `kalshi bot
status`): read the evidence file → `parse_latest` → `cli_report` → print. Read-only.

**3. Bot cockpit line** — `BotCockpitPresentation` (or a sibling) calls `parse_latest`
+ `cockpit_line` and renders it as one **advisory-labelled** line in the scene, beside
the gate, never touching the gate's decision. UNAVAILABLE/STALE fail closed exactly as
the ledger KPIs do.

## Honesty guarantees (baked in)

- Every survivor AND candidate row carries `effective_n`, both deltas, and `trust`;
  a candidate additionally carries `blocked_by` so its shortfall is never hidden.
- A missing/stale/garbage file reads `UNAVAILABLE`/`STALE`, never a confident blank or
  a zero.
- The line can only claim `measured` for a variant the engine's own gate (beats both
  nulls, BH, walk-forward, effective_n≥30) already blessed — the consumer cannot
  upgrade a candidate to a winner.
- Candidates are ranked by the *harder* null (`delta_vs_market`) and floored at
  `effective_n≥10`, so "forming edge" means a positive, real-sample near-miss — not a
  noisy or negative slice dressed up.

## Testing

- `KalshiStrategyGridView` is pure → QtTest: parse (valid / missing file / garbage /
  schema mismatch / stale), and each formatter (no-survivor-no-candidate,
  candidate-only, survivor, UNAVAILABLE, STALE) — asserting the humility fields and
  `blocked_by` appear.
- Grid `candidates[]`: extend `tests/test_strategy_grid.py` — a seeded run with a
  positive near-miss yields a candidate with the right `blocked_by`; a negative slice
  never becomes a candidate.
- CLI verb: a small e2e (seed a `-latest.json`, run the verb, assert output + `--json`).
- Cockpit line: a presentation test.

## Rollout

1. `strategy_grid.py` `candidates[]` emission + tests (Python; pure).
2. `KalshiStrategyGridView` pure core + QtTest (C++).
3. CLI verb + registration + e2e.
4. Bot cockpit advisory line + presentation test.

## Queued next (separate feature, not this plan)

**The maker quote-lag engine** — the one real structural edge (book lags spot ~2¢/15s)
captured as a *maker* (rest the quote, pay no half-spread), behind an honest fill model
(did the market trade through the resting price? queue position). This is the actual
candidate winner; it gets its own brainstorm → spec → plan.
