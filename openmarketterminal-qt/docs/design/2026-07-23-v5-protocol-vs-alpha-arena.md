# Forecasting duels vs. trading arenas: the v5 protocol compared to Alpha Arena

*Open Terminal design note — 2026-07-23*

## Summary

Alpha Arena (Nof1.ai) made "frontier models competing with real money" a public
category in late 2025. Open Terminal independently built the same category of
experiment — a blind, paired, Claude-vs-Codex forecasting duel on Kalshi hourly
crypto contracts — and arrived at a materially stricter measurement design.
This note compares the two methodologies and argues one structural point:
**prediction markets are the missing venue in the AI-trading-arena landscape.**
Perpetual-futures PnL is a compound of forecasting, sizing, timing, and risk
management measured over a handful of path-dependent weeks; prediction-market
contracts decompose to the one thing a benchmark can actually score cleanly —
calibrated probability — settled objectively, hundreds of times per week.

## The two designs at a glance

| Dimension | Alpha Arena (S1/S1.5) | Open Terminal duel (v4 → v5) |
|---|---|---|
| Venue | Hyperliquid crypto perps (S1); US equities (S1.5) | Kalshi hourly BTC/ETH contracts |
| What is scored | Raw PnL on $10k real capital | Brier score of committed probabilities at settlement |
| Sample unit | One multi-week equity curve per model | One jointly-resolved paired contract (target ≥ 200) |
| Contestants | 6–8 frontier models, spectator-only | Claude (claude-opus-4-8) vs Codex (gpt-5.6-sol), pinned versions, no fallback |
| Inputs | Identical prompts + full market data (price, history) | Byte-identical prompts + **price-free blind packet** (strikes, distance, vol, microstructure — market odds withheld until reveal) |
| Fairness proof | Asserted (prompts not published) | Per-row `prompt_hash` + `context_hash`; commit-blind sealing (`sealed_hash` binds context + withheld market + nonce); INVALID_EPOCH on any divergence |
| Decision rule | Leaderboard at season end | **Preregistered**: winner only with ≥200 jointly-resolved pairs, ≥80% coverage for BOTH, paired-Brier bootstrap CI excluding zero; otherwise no winner |
| Selection effects | None addressed | Coverage gate — v4 detected timeout-censoring of hard (near-strike) cases and correctly refused to declare a winner |
| Mid-run integrity | Not addressed publicly | Scoring-infrastructure SHA-256 journaled per opportunity; any mid-epoch change ⇒ INVALID_EPOCH |
| Latency fairness | Not addressed | v5 symmetric budgets (88s internal / 90s supervisor / 100s TTL), identical for both lanes, timeout taxonomy journaled |
| Execution authority | Models place real orders | Models are advisory-only shadow forecasters; a deterministic executor holds sole trading authority |
| Capital at risk | Real ($10k/model S1; $320k S1.5) | None in the duel lane (real capital gated separately behind the engine's own scoreboard) |
| Transparency | Trades + "ModelChat" public; prompts private | Full protocol in-repo; hashes auditable; Forecast Arena UI shows epoch pair, coverage, integrity status |

## What Alpha Arena got right

Credit where due — three things the arena format proved:

1. **Real capital forces execution honesty.** Slippage, funding, liquidation
   risk, and position sizing are real skills a probability-only duel does not
   measure. Season 1's blowups (three US models down 45–63%) were largely
   risk-management failures, not forecasting failures — that is a finding.
2. **Spectacle drives scrutiny.** Public equity curves and per-decision
   reasoning notes invited the methodological criticism that made the whole
   category sharper. Season 1.5's "multiple themed competitions for statistical
   rigor" is a direct response.
3. **Fixed seasons with pinned models** made results at least nominally
   attributable to specific model versions.

## Where the v5 protocol is stricter

1. **Scoring rule.** PnL over 17 days is one draw from a fat-tailed
   distribution; leverage choices dominate forecast quality. Brier over
   hundreds of independently settled binary events is a proper scoring rule
   with usable statistics. v5's decision rule is preregistered and mechanical —
   there is no discretion after seeing results.
2. **Blindness.** Alpha Arena models see prices — so a model can simply lean on
   momentum or market-implied information. The duel's blind packet strips every
   market-priced field (allowlist-only copy, forbidden-key regression tests);
   the model must produce a probability from physics-like features (distance to
   strike, realized vol, time, microstructure). Commit-blind hashes prove the
   forecast predated the reveal.
3. **Selection effects are a first-class failure mode.** v4's headline result
   (Claude Brier 0.301 vs Codex 0.483) was *refused* by the protocol because
   Claude's 62% coverage was easy-biased — its timeouts clustered on hard
   near-strike cases. No arena-style leaderboard would have caught this; the
   coverage gate did, and v5's latency-neutral budgets exist because of it.
4. **The referee is frozen.** Every opportunity journals a SHA-256 of the
   scoring implementation (report, prompt builder, both forecaster wrappers,
   settlement math); any midstream change invalidates the epoch automatically.
   Alpha Arena's harness is private and mutable — the standing critique that
   results reflect harness, not model, cannot be answered from outside.
5. **Difficulty is measurable.** Because contracts have strikes and the packet
   carries realized volatility, every forecast has an objective hardness
   (required move in sigmas). Scoring can be cohorted by difficulty. An equity
   curve has no such decomposition.

## The thesis: predictions are the missing venue

Every 2026 arena platform — Alpha Arena, TradeRank, BingX, RockAlpha,
StrategyArena — runs on directional trading of continuous assets. None runs on
prediction markets. That is a gap, because prediction markets are arguably the
*best* venue for benchmarking model intelligence:

- **Objective, fast settlement.** An hourly contract resolves YES/NO against a
  published reference within the hour. No mark-to-market ambiguity, no
  path-dependence, no liquidation luck.
- **Proper scoring rules apply.** Brier/log scores measure calibration
  directly. "Was your 70% actually a 70%?" is answerable; "was that long a
  good trade?" mostly is not.
- **Sample size scales.** Hundreds of independent resolutions per week per
  underlying vs. one equity curve per season. Statistical significance is
  reachable in weeks, honestly.
- **Skill decomposes.** Forecasting is isolated from sizing and execution. A
  venue that wants to test the full stack can add a sizing layer *on top of*
  scored probabilities — the reverse decomposition is impossible.
- **Difficulty is intrinsic.** Distance-to-strike over realized vol gives every
  event a hardness score; leaderboards can show skill on hard cases, where it
  actually matters.
- **Real money still available.** Kalshi/Polymarket contracts let an arena keep
  the real-capital stakes that make Alpha Arena compelling, without leverage
  blowups dominating the signal.

The obvious synthesis — an "Alpha Arena for predictions": pinned frontier
models, identical price-free packets, commit-blind probabilities on the same
paired contracts, preregistered coverage/sample/CI gates, frozen scoring
infrastructure, public hashes, optional fixed-rule staking for a real-money
scoreboard. Open Terminal's v5 protocol is, in effect, a two-model instance of
exactly that design, running now.

## Status and disclosure

The v4 epoch ended INSUFFICIENT_PAIRED_DATA by its own preregistered rule
(coverage gate), with Claude leading on the shared easy cases — an honest
non-result that the protocol itself surfaced. The v5 epoch (latency-neutral
budgets, scoring freeze, vol-carrying context) is the live rematch. This note
was written by Claude, one of the two contestants' vendors' models; the
protocol's mechanical decision rule and auditable hashes exist precisely so
that authorship of prose cannot tilt outcomes.

## Sources

- nof1.ai — Alpha Arena site and leaderboards
- Datawallet, "Alpha Arena (Nof1 AI) Explained" — mechanics, cadence, ModelChat
- ForkLog / RootData — Season 1 results (Qwen 3 Max +22.31%; four US models −31…−63%)
- PANews — Season 1.5 ($320k, equities, Kimi K2 + mystery model)
- TradeRank, "Alpha Arena Alternatives (2026)" — landscape, prompt-transparency critique, spectator-only status
- In-repo: `2026-07-21-claude-vs-codex-blind-forecasting-competition-design.md` (v5 amendments), competition plan, scoring-freeze implementation (PR #70)
