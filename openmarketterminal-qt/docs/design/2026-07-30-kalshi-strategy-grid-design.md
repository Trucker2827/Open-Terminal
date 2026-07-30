# Kalshi paper strategy-grid — design

**Date:** 2026-07-30 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-strategy-grid` (off `main`)

## Why

The exit-strategy investigation (`exit_vs_hold`, `cashout_timing`, `path_exit_sim`)
and the real-account autopsy answered "does cashing out beat holding?" on small,
after-the-fact samples. The operator asked the natural generalization: *why can't a
paper engine run **every** scenario — buy YES/NO at low/high probability, cash out
early or hold — and just tell me which works?* It can. Now that the retained series
keeps the real book (hourly since #170, 15-minute as of this week), a reproducible
sweep can replay a **pre-registered grid** of mechanical + signal-gated paper
strategies over every retained contract, marked to the real recorded book and settled
on real outcomes net of fees.

The value is not "find the winner" — with hundreds of variants some will look great by
chance. The value is a **disciplined, machine-readable verdict**: for each strategy,
its measured edge against the honest nulls, its effective sample size, and whether it
survives multiple-comparison correction and walk-forward. That verdict becomes a
first-class evidence artifact the CLI, the bot, and any agent consume — measured trust
flowing into the deterministic engine, never an autonomous trade trigger.

## Goals & non-goals

**Goal:** a read-only, reproducible periodic sweep producing per-variant paper
scoreboards + a machine/CLI/bot-consumable verdict file, with the statistical
discipline that stops the grid from flattering itself.

**Non-goals / never:**
- **No maker/resting variants** in v1 (taker fills only — maker is where paper lies
  about fills; deferred to v2 behind a real fill model).
- **No live in-app tick-by-tick engine** in v1 (the "sweep now, live later" decision;
  the retained series *is* the recorded live book, so the sweep is identical but
  reproducible). A thin live preview is a later follow-on.
- **No autonomous order path, ever.** The grid measures and publishes; the bot stays
  deterministic and advisory. Consumers surface/weight the grid's verdicts; nothing
  auto-trades them.

## The pre-registered grid

Fixed in code before any run — no post-hoc knob tuning. A *variant* is the tuple:

- **side** ∈ {YES, NO}
- **entry_band** (on the bet-side price) ∈ {2–10, 10–25, 25–50, 50–75, 75–90, 90¢+}
- **entry_gate** ∈ {mechanical, physics, calibrator}
- **exit** ∈ {hold, TP+{5,10,15,20,30}¢, SL−{5,10,15,20}¢, trail−{5,10,15}¢}

Every variant is assigned a **stable `variant_id`** (deterministic from the tuple) so
scoreboards are comparable across runs and consumers can reference a variant durably.

## Fill & cash-out semantics (taker only)

- **Entry:** first tick whose bet-side ask ∈ band, with ≥ `MIN_ENTRY_S_TO_CLOSE` left
  and (for gated variants) the gate satisfied at that instant. Buy at the ask.
- **Cash-out / exit:** sell at the real bid when the exit rule fires (exactly Kalshi's
  cash-out button — a taker hit, always achievable). Else settle.
- **Fees:** Kalshi `ceil(0.07·P(1−P)·100)¢` on both legs (shared `fee_per_contract`).
- P&L identity reused from `exit_vs_hold`: the buy leg cancels in the paired diff, so
  exit-vs-hold reduces to `b − fee(b) − outcome`.

## Signal gates (no look-ahead)

- **mechanical:** no gate — enter on first band touch.
- **physics** (recomputed deterministically from retained BRTI+book at the entry
  instant, so the sweep is reproducible): distance-from-strike in σ, Gaussian
  implied-prob vs mid, and post-spot-move quote-lag. Reuses
  `kalshi_edge_common.implied_probability` / `realized_vol_per_min_bps` and the
  `q1_quote_lag` event logic.
- **calibrator:** the nearest logged `calibrated_p` at/before the entry instant from
  `kalshi-bot-decisions.jsonl`, within a freshness bound; gate fires when
  `calibrated_p` vs mid clears a pre-set edge threshold. A contract the log cannot
  cover at entry is **counted as `calibrator_ungateable`, never imputed** (the log's
  coverage gaps are reported, per the autopsy's honesty about them).

## Population & outcomes

- **Families:** `KXBTCD` hourly threshold (rich history) + `KXBTC15M` 15-minute
  directional (accruing). `KXBTC` band markets excluded (YES non-monotone in spot),
  counted.
- **Outcomes:** recorded settlement first; validated BRTI-derivation fallback for
  `KXBTCD` (strike present); `KXBTC15M` **recorded-only** (no strike to derive from).
  Derivation agreement quoted, per `validate_settlement_rule`.

## Statistical discipline (the heart)

Every variant is scored against **two nulls, never zero**: the HOLD baseline and the
market mid. Reported per variant:

- `n_contracts`, and **`effective_n`** via clustering by settlement-window /
  triggering spot-move (contracts sharing one BTC move are not independent — the
  autopsy's core lesson).
- mean/total net P&L, ROI, win-rate; `delta_vs_hold`, `delta_vs_market`.
- **clustered** SE + t (move-clustered, ~1.7× naive per f1), not just naive t.
- **walk-forward**: train/test split by close time; a variant must persist
  out-of-sample or it is not a "survivor."
- **multiple comparisons stated out loud:** the full grid is emitted; the run reports
  how many variants beat a null at nominal α vs after **Benjamini–Hochberg** (and
  Bonferroni for reference); a variant is flagged `survivor` only if it clears
  correction **and** walk-forward **and** has adequate `effective_n`.

The headline is allowed to be "no variant beats hold-and-market after correction" —
that is a real, useful answer (and the autopsy's prior expectation).

**Pre-registered parameters** (literals in the script, fixed before any run — changing
them is a code change with a reason, never a per-run knob):
`MIN_ENTRY_S_TO_CLOSE = 120`s; walk-forward = 5 expanding folds by close time;
multiple-comparison correction = Benjamini–Hochberg at α = 0.05 (Bonferroni reported
alongside for reference); **survivor floors**: `effective_n ≥ 30` AND clears BH AND
`walkforward_delta` keeps the sign of the pooled edge; calibrator gate fires when
`|calibrated_p − mid| ≥ 0.03` on the bet side; calibrator freshness bound = 60 s (a
logged prediction older than this at the entry instant is `calibrator_ungateable`).

## Output artifacts (the evidence bus)

1. **`kalshi-strategy-grid.json`** — the full grid. `schema_version`, `as_of_utc`,
   data provenance (files, row counts, spans), method block, and `variants[]` each:
   `{variant_id, side, entry_band, entry_gate, exit, n_contracts, effective_n,
   win_rate, mean_pnl, total_pnl, roi, delta_vs_hold, delta_vs_market, clustered_t,
   ci95, walkforward_delta, survives_correction, verdict}`. Self-describing and
   versioned for forward-compatible consumers.
2. **`kalshi-strategy-grid-latest.json`** — the compact machine/bot/CLI-facing summary:
   `schema_version`, `as_of_utc`, a one-line `headline`, and `survivors[]` (only
   variants clearing correction + walk-forward + effective_n), each carrying its
   measured edge, `effective_n`, `ci95`, and an explicit **`trust` ∈
   {measured, insufficient_sample, rejected}**. When nothing survives, `survivors: []`
   and the headline says so. This is the cheap file consumers read — the honesty
   metadata travels *with* every record so no consumer can act on a nominal fluke.
3. **`docs/research/2026-…-kalshi-strategy-grid.md`** — the human report (autopsy style).

## Consumers (read-only; measured-trust-in, advisory-out)

- **CLI:** a read-only verb (e.g. `openterminal kalshi strategy-grid [--json]`) that
  prints `-latest.json`'s headline + survivors. No recompute; a thin reader.
- **Bot:** reads `-latest.json` as **labeled advisory evidence beside its own gate** —
  e.g. the bot/cockpit panel can show "grid: band 10–25¢ NO + SL−15¢ measured +3.7¢,
  eff-n 42, survives correction" or "grid: no measured edge." It **never** auto-acts;
  a possible v2 is a *conservative advisory gate input* (e.g. flag entries in bands the
  grid measures as net-negative with high confidence), still advisory, still
  human/deterministic-gated.
- **Machine / agents (MCP):** the versioned JSON lives in the evidence dir; existing
  file/MCP tools read it. A dedicated MCP verb is a later nicety, not v1.

Guardrail restated: because every consumable record carries `effective_n`, `ci95`, and
`survives_correction`/walk-forward status, "the bot benefits" can never quietly become
"the bot chases noise." Consumers that ignore the humility metadata are the bug, and
the schema makes that omission visible.

## Reuse & reproducibility

Read-only `scripts/research/strategy_grid.py`, reusing `kalshi_edge_common`
(loaders, fee, outcomes, validation) and the `path_exit_sim` / `q1_quote_lag`
primitives (quote series, exit simulation, event detection). Emits stamped JSON +
report; runnable now; schedulable via launchd later. Point at a frozen copy via
`OPENTERMINAL_EVIDENCE_DIR`. Methodology guarantees regression-tested
(`tests/test_strategy_grid.py`): stable `variant_id`s, no-look-ahead gating,
clustered effective_n, walk-forward split by close time, correction arithmetic, and
`-latest.json` carrying the humility fields.

## Rollout

1. Core sweep + mechanical grid + two nulls + tests (KXBTCD first; 15M folds in as it
   accrues).
2. Physics + calibrator gates (+ coverage accounting) + tests.
3. Statistical layer: clustering/effective_n, walk-forward, multiple-comparison
   correction + tests.
4. Evidence artifacts: full JSON + `-latest.json` (versioned schema + trust labels) +
   human report.
5. Consumers: CLI reader verb; bot advisory panel line. (Live preview + MCP verb +
   maker variants are explicit follow-ons.)

## Amendments during implementation (2026-07-30)

Recorded so this spec matches the shipped `scripts/research/strategy_grid.py` (final review):

- **Survivor rule now requires beating BOTH nulls, positively.** A variant is
  `trust: "measured"` iff it clears Benjamini–Hochberg AND `effective_n ≥ 30` AND
  **`delta_vs_hold > 0` AND `delta_vs_market > 0`** AND `sign(walkforward_delta) ==
  sign(delta_vs_hold)`. The earlier rule only checked the sign match, which could
  have flagged a reliably-LOSING variant (two-sided-significant, negative edge,
  negative walk-forward) as a survivor. Requiring both deltas positive makes the
  `-latest.json` "beats hold+market" headline literally true. Note the market null
  is structurally harder: `market_pnl` uses the entry mid, so `delta_vs_market <
  delta_vs_hold` almost always (by ~half the spread).
- **Bonferroni is reported** (design pre-registration) as
  `data.bonferroni_significant_count` (variants with `p < alpha/m`); it is a
  reference anchor and does not gate.
- **Walk-forward is a single time-ordered final holdout, not 5 separate folds.**
  `_fold_bounds` computes 5 expanding boundaries but the sign check uses the last
  (~final 20% of contracts by close time) as the out-of-sample test. This realises
  the "train/test split by close time" intent; the literal "5 folds" is not
  implemented (on small samples the holdout can hold few clusters — a known
  weakness of the sign gate until more data accrues).
- **Full-grid JSON keys.** Each variant record uses `band`/`gate` (not
  `entry_band`/`entry_gate`), carries `trust`, and reports `delta_vs_hold`/
  `delta_vs_market`/`effective_n`/`ci95`/`walkforward_delta`/`win_rate`/`p_value`/
  `ungateable`/`survives_correction` rather than `mean_pnl`/`total_pnl`/`roi`/
  `clustered_t`. Consumers read `-latest.json`, whose schema is as designed.
- **`run(evidence)` param is currently unused** (all loaders resolve paths via
  `OPENTERMINAL_EVIDENCE_DIR`); kept for signature stability, documented as a
  footgun to wire through or drop when a caller needs a non-default dir.
