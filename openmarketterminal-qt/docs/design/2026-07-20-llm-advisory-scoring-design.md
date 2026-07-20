# LLM Advisory Scoring — Firewalled Prediction & Attribution

**Date:** 2026-07-20
**Status:** Design (approved for planning)
**Scope:** Kalshi autonomous control plane (`kalshi auto …`), additive and off the execution path.

## Problem

The deterministic settlement model (`source='kalshi auto-plan'`) is scored six ways —
`kalshi auto attribution`, `calibration`, and `cohort-significance` compute Brier vs market,
reliability, and cohort significance over resolved `edge_decision_journal` rows. The LLM
advisory layer is labeled `advisory_only` everywhere but is **never captured as a resolvable
record, never resolved against outcomes, and never scored.** It is advisory in name and
invisible to measurement.

We cannot answer the only question that justifies an LLM in the loop: **does acting on the LLM
add edge over the deterministic daemon, or does it only add latency?** Worse, without a
disciplined capture protocol, any naive attempt to answer it is invalid — the LLM consumes a
stream carrying `market_implied_probability`, so a probability formed *after* seeing the quote
tracks the market by construction, and a scoring surface that only sees *committed* predictions
is corrupted by selection bias.

## Goal

A defensible experiment that measures, out-of-sample and after fees, whether a named forecaster
(a specific LLM/prompt, or another agent) has predictive skill on Kalshi crypto settlement —
independently of the market, and marginally over the daemon. The deliverable is an **honest
answer with confidence intervals**, not a profit engine. Advisory authority is never a config
flag; promotion to any execution influence is a separate, gated, deterministic step.

## Non-goals

- No change to the deterministic daemon's execution authority. It continues to exclusively own
  `execute-next → prepare_order → submit_order`.
- No LLM order creation, ever, from this feature. The terminal promotion rung is a bounded
  weighting / veto, never sole execution authority.
- No new alpha model. This measures a forecaster; it does not build one.

## Design decisions (locked)

1. **Firewalled estimate.** The forecaster commits its probability *blind to price*.
2. **Two-step challenge protocol**, extended to a 5-state blind/reveal/post protocol, enforces
   and timestamps the firewall across separate one-shot CLI invocations.
3. **Four-way paired scoring** — `p_pre`, `p_post`, `market`, `daemon` — with the **headline =
   out-of-sample paired improvement over daemon and market, with CIs, coverage, and net
   executable value after fees.**
4. **`spot_microstructure` (Coinbase/Gemini/Kraken aggressor flow) is in the blind packet** — it
   is underlying-exchange-derived, not the Kalshi market, and is a legitimate independent
   predictor.

## Protocol — five states, two estimates

The headless CLI is one-shot: each verb is a separate process, so all state is durable in the
DB. A single challenge flows:

```
open          → build price-free blind context; seal withheld market+daemon; issue challenge
commit-blind  → permanently record p_pre against the sealed, contemporaneous market@open
reveal        → publish sealed_hash; reveal Kalshi quote, divergence, daemon prob, contract flow
commit-post   → record p_post (market-informed)
resolve       → settlement backfills outcome; scoring computes p_pre, p_post, market, daemon
```

This measures four things:
- **Independent predictive information** — `Brier(p_pre)` vs `Brier(market@open)`.
- **Whether seeing the market helps or hurts** — `Brier(p_post)` vs `Brier(p_pre)`.
- **Improvement over the daemon** — paired `Brier(p_pre|p_post)` vs `Brier(daemon)`.
- **Whether disagreement is signal or noise** — realized value on the subset where
  `sign(p_llm − market) ≠ sign(p_daemon − market)`.

### CLI surface

```
kalshi auto advise open   --ticker T [--horizon H] [--forecaster-id ID]
                          [--provider P --model M --prompt-version V
                           --context-schema-version N --temperature X
                           --agent-id A --run-id R]
  → {challenge_id, ticker, context:<price-free>, context_hash,
     prediction_ttl_ms, execution_relevance_ttl_ms, ts_opened, PRICE_WITHHELD:true}

kalshi auto advise commit-blind --challenge ID --commit-id K --probability P
                          [--confidence C] [--rationale "..."]
  → {state:COMMITTED_BLIND, id, ts_committed}     # price still withheld

kalshi auto advise reveal --challenge ID
  → {state:REVEALED, sealed_hash, market_at_commit, daemon_probability,
     divergence, kalshi_flow, cost_net_edge}

kalshi auto advise commit-post --challenge ID --commit-id K2 --probability P2
                          [--confidence C2] [--rationale "..."]
  → {state:COMMITTED_POST, id}

kalshi auto advise score  [--forecaster-id ID] [--horizon H] [--limit N] [--json]
  → paired OOS report (see Scoring)

kalshi auto advise ledger [--state S] [--limit N] [--json]   # participation audit
```

`commit-post` is optional per challenge; a challenge that reveals but never post-commits
terminates `ABANDONED` (recorded, counted). `commit-blind` may be skipped only by abandoning the
challenge (also recorded).

### Blind packet allowlist (verified against `ServeCommand.cpp`)

**Included (blind):** strike floor/cap, distance from spot, `required_move_bps`, `seconds_left`,
settlement band + settlement definition, horizon, spot level, realized recent move, realized-vol
estimate, and `spot_microstructure` (Coinbase/Gemini/Kraken aggressor pressure/coverage).

**Excluded from blind, revealed only at `reveal`:** Kalshi `yes/no bid/ask/depth`,
`market_implied_probability`, `fair_yes/no`, `divergence` (built from `contract_change_cents` —
`ServeCommand.cpp:1128` — leaks contract price action), Kalshi contract flow (`:203`), daemon
`calibrated_probability` / `model_probability` / `model_weight`, cost/fee derived from the quote.

**Honest limit (documented, not blocked):** the protocol firewalls *within a transaction* and
timestamps commit-before-reveal, but cannot prevent an operator who read the quote via a
different command first. That residual is logged (via forecaster identity + timestamps), not
prevented — the tradeoff accepted when choosing the protocol over a separate-credential
architecture.

## Data model

### New table — `edge_advisory_challenge` (durable, never pruned)

Retaining every challenge — including `EXPIRED` and `ABANDONED` — is what defeats selection
bias. Columns (indicative):

```
challenge_id           TEXT PRIMARY KEY
state                  TEXT   -- OPEN|COMMITTED_BLIND|REVEALED|COMMITTED_POST
                              --  |EXPIRED|ABANDONED|INVALIDATED
created_at             INTEGER
prediction_ttl_at      INTEGER   -- commit deadline
execution_relevance_at INTEGER   -- shorter; past it, scored but never trade-influencing
ticker, market_id, horizon, settlement_def   TEXT
context_json           TEXT   -- price-free blind packet (canonical)
context_hash           TEXT   -- SHA256(canonical(price_free_context)), returned at open
sealed_hash            TEXT   -- SHA256(canonical(context+withheld+metadata+nonce)), published post-reveal
nonce                  TEXT
-- contemporaneous baselines captured at each transition:
market_at_open_json, market_at_blind_json, market_at_reveal_json, market_at_post_json  TEXT
   -- each: yes/no bid/ask/depth + est. fees + market_implied_probability
daemon_prob_at_open, daemon_prob_at_reveal   REAL
-- forecaster identity:
provider, model, prompt_version, context_schema_version, protocol_version,
agent_id, run_id     TEXT
temperature          REAL
-- estimates:
p_pre, p_post        REAL      -- -1 until committed
confidence_pre, confidence_post   REAL
rationale_pre, rationale_post     TEXT   -- concise; NO hidden chain-of-thought
commit_id_blind, commit_id_post   TEXT   -- idempotency keys
ts_blind, ts_reveal, ts_post      INTEGER
```

### Reused table — `edge_decision_journal`

On `commit-blind`, insert one resolvable row so the existing settlement backfill resolves it:

- `source = 'llm-advisory'`
- `model_probability = p_pre`  (headline independent estimate, for existing Brier code)
- `market_probability = market_implied_probability @ open`
- `side`, `confidence`, `market_id`, `seconds_left`, `outcome = -1`
- `features_json` carries the full four-way + identity + integrity + separation flags:

```json
{
  "model_version": "llm-advisory-v1",
  "protocol_version": 1, "context_schema_version": 1,
  "challenge_id": "...", "context_hash": "...", "sealed_hash": "...",
  "ts_opened": 0, "ts_blind": 0, "ts_reveal": 0, "ts_post": 0,
  "horizon": "hourly", "settlement_def": "...",
  "p_pre": 0.62, "p_post": 0.58,
  "market_at_open": 0.55, "market_at_post": 0.56,
  "daemon_probability": 0.55,
  "forecaster": {"provider":"...","model":"...","prompt_version":"...",
                 "temperature":0,"agent_id":"...","run_id":"..."},
  "authority": "advisory_only", "execution_eligible": false,
  "gate": "measurement_only", "call": "LLM_ADVISORY"
}
```

**Integration point to verify at plan time:** confirm the settlement backfill
(`UPDATE edge_decision_journal SET outcome=…, resolved_at=…`) is **not** scoped to
`source='kalshi auto-plan'`. If it is, widen it so `llm-advisory` rows resolve on the same CF
Benchmarks settlement — no new resolution logic, just coverage.

**Semantic separation:** every advisory row is stamped `authority:advisory_only`,
`execution_eligible:false`, `gate:measurement_only`, `call:LLM_ADVISORY`. The existing
deterministic/attribution queries filter `source='kalshi auto-plan'` and the settlement
`model_version`, so they never pick up advisory rows; the flags are explicit belt-and-suspenders.

## TTL

`ttl = min(configured_max, horizon_limit)`, `configured_max` default **60s**.

| Settlement remaining | Challenge TTL |
|---|---|
| ≤ 60 s | do not open, or max 10 s |
| 1–5 min | 15 s |
| 5–15 min | 30 s |
| 15–60 min | 45 s |
| > 60 min | 60 s |
| absolute max | 120 s |

Two clocks: `prediction_ttl` (deadline to `commit-blind`) and the shorter
`execution_relevance_ttl`. A prediction committed after execution relevance expires is journaled
and scored academically but is flagged **never trade-influencing**.

## Commit semantics

Each commit is a single DB transaction: lock/read challenge → verify state + expiry → write the
journal row → mark challenge state → store the contemporaneous revealed data → commit
atomically. A client-supplied `commit_id` idempotency key makes retries safe — a duplicate
`commit_id` returns the original result and never creates a second prediction.

The journal row is **created at `commit-blind`** (with `p_pre`, `market@open`, `daemon@open`,
`outcome=-1`) and **finalized once at `commit-post`** (appending `p_post` and `market@post` to
`features_json`). The pre-reveal fields are immutable after `commit-blind`; the post fields are
append-once at `commit-post`. A challenge that never post-commits leaves a valid, scorable row
carrying `p_pre` only. `outcome` is written exactly once by the settlement backfill at `resolve`.

## Cryptographic binding

`context_hash = SHA256(canonical(price_free_context))` is returned at `open`.
`sealed_hash = SHA256(canonical(price_free_context + withheld_market + metadata + nonce))` is
retained internally and **published at `reveal`**. Canonical serialization = sorted-key,
fixed-precision JSON. Any later mutation of the sealed context or withheld baselines is
detectable by recomputation.

## Scoring — paired, out-of-sample

Over resolved rows for a `forecaster-id` (and cohort filters):

- Brier, log loss, reliability / calibration error, Murphy resolution & discrimination.
- `market` and `daemon` Brier computed on the **exact same sample**.
- Paired improvement `Brier_baseline − Brier_llm` for baseline ∈ {market, daemon}, for
  estimate ∈ {p_pre, p_post}.
- **Bootstrap confidence intervals** on each paired improvement.
- Coverage and **abstention-adjusted** results (an abstained challenge counts against coverage,
  not silently dropped).
- Net-of-fees value using the **quote available at commit** (fee curve
  `ceil(0.07·C·p(1−p))`), not the stale open quote.
- Breakdown by settlement-band × distance-to-strike cohort.
- **Participation audit** from the challenge ledger: open→commit rate, expiration rate,
  selective-participation skew by confidence / direction / regime. Results are reported over
  **all eligible challenges**, not only committed ones.

**Headline metric:** out-of-sample paired improvement over daemon *and* market, with CIs,
coverage, and net executable value after fees.

## Trial gate (all required)

- Minimum resolved out-of-sample sample size.
- Positive paired improvement versus the daemon.
- Confidence interval excluding zero.
- No severe calibration failure.
- Positive simulated net value after fees / slippage.
- Adequate participation coverage (low, non-selective abstention).
- Performance across more than one market regime.
- Zero firewall-integrity violations (hash mismatch, commit-before-open, price-in-blind-packet).

Even after passing, promotion proceeds one rung at a time and **never** reaches sole execution
authority:

```
measurement-only → optional abstention/veto → bounded weighting → (never sole authority)
```

## Error handling & edge cases

- No fresh daemon snapshot / missing market@open → do **not** open a challenge (a comparison
  without a contemporaneous market baseline is void).
- `probability ∉ [0,1]` → reject.
- Challenge expired at commit → mark `EXPIRED`, reject the commit (recorded, not lost).
- Duplicate `commit_id` → idempotent replay of the original result.
- `reveal` before `commit-blind` → reject (would break the firewall).
- `commit-post` without prior `reveal` → reject.
- Abandoned challenge (opened/blind-committed, never progressed past TTL) → `ABANDONED`, counted
  in participation stats.
- `INVALIDATED` reserved for operator/integrity voids (e.g. detected sealed-hash mismatch).

## Testing (verification discipline — throwing checks, regression-first)

- **Firewall leak test (load-bearing):** assert the `advise open` JSON contains **none** of
  `{yes_bid, yes_ask, no_bid, no_ask, market_implied_probability, fair_yes, fair_no, divergence,
  daemon_probability, model_weight}`. Fails (throws / non-zero exit) if any price field leaks.
- **Commit journaling:** `commit-blind` writes `source='llm-advisory'`, correct `p_pre`,
  `market@open`, `daemon_probability`, `outcome=-1`, all separation flags; `ts_blind > ts_opened`.
- **Idempotency:** duplicate `commit_id` yields one row.
- **Lifecycle:** expired challenge rejected + state `EXPIRED`; `reveal`-before-blind rejected;
  double state transitions rejected.
- **Resolution:** a settled market backfills the `llm-advisory` row's `outcome`.
- **Scoring math:** synthetic resolved rows → known Brier / log-loss / paired improvement /
  bootstrap CI bounds (RED without the scoring code).
- **Participation/selection:** a ledger with abandoned + committed challenges → coverage and
  skew reported over all eligible challenges.
- **Integrity:** mutating a stored sealed context is detected on `sealed_hash` recompute.

Tests use throwing / exit-nonzero checks (not `assert`, a no-op under NDEBUG). Every fix ships a
regression test that fails without it.

## Build surface (for planning)

1. Migration: `v0xx_edge_advisory_challenge` (durable ledger).
2. Commands: `advise open | commit-blind | reveal | commit-post | score | ledger`.
3. Journal write on `commit-blind`; verify/widen settlement backfill to cover `llm-advisory`.
4. Scoring module (paired OOS + bootstrap + participation audit).
5. Tests per above.

All additive; no change to the deterministic execution path or the daemon's authority.
