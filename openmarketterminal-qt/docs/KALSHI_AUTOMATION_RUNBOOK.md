# Kalshi automation runbook

OpenTerminal's Kalshi lane is paper-first and fail-closed. The automated micro-market strategy uses fill-and-kill limit orders, exchange client IDs, an authenticated reconciliation loop, and a durable order/fill ledger. An accepted or resting order is never counted as a fill.

The persistent daemon also builds the same Coinbase/Kraken/Gemini microstructure snapshot shown by `edge flow`: source freshness, cross-exchange divergence, top-book imbalance, microprice, multi-window tape pressure, and buyer-versus-seller initiated executed volume. Coinbase maker side is inverted to obtain taker/aggressor side; Gemini `makerSide` is inverted; Kraken's trade side is already the taker side. The CLI reports classified-volume coverage so a thin or partially classified sample is visibly marked as warming. A Kalshi leg can execute only when at least two fresh spot books agree and their direction confirms the selected YES/NO side. Credible opposing aggressor flow is an additional veto. This layer is confirmation-or-veto only; it cannot create model edge or bypass the normal Kalshi risk gates.

## Promotion sequence

1. Configure Kalshi **demo** credentials and confirm `kalshi auto positions --json` returns without an account error.
2. Run the paper daemon and review `kalshi auto status`, `kalshi auto audit`, and `kalshi auto calibration` over a representative sample. Model authority remains market-only until the configured out-of-sample evidence threshold is earned.
3. Exercise a bounded demo session with the same limits intended for production. Verify order states, partial fills, fees, and positions after restarting the CLI/daemon.
4. Test emergency handling with `kalshi auto live session stop` followed by `kalshi auto cancel-orders --yes`. The latter cancels only order IDs owned by the local Kalshi execution ledger and verifies that none remains resting.
5. Only then configure production credentials, retain the default $2 order and $120 experiment caps, and arm a one-clock-hour session. Increase limits only after reconciling a statistically meaningful production sample.

## Operator commands

```text
openterminalcli --json --headless kalshi auto status
openterminalcli --json --headless kalshi auto decisions
openterminalcli --json --headless kalshi auto candidates
openterminalcli --json --headless kalshi auto positions
openterminalcli --json --headless kalshi auto live session 1h
openterminalcli --json --headless kalshi auto live session stop
openterminalcli --json --headless kalshi auto cancel-orders --yes
```

For external bots and live LLM observers, use the versioned newline-delimited stream. Every line is a complete `schema_version: 1` JSON document; no terminal-table parsing is required:

```bash
openterminalcli --headless edge flow BTC-USD --sources coinbase,gemini \
  --watch --jsonl --interval-ms 500 --duration-ms 4000
```

The LLM boundary is explicit in every Kalshi decision snapshot: an LLM is advisory-only, while the deterministic daemon exclusively owns `execute-next -> prepare_order -> submit_order`. It rechecks snapshot freshness, two-source agreement, quote economics, contract exposure, session authority, and risk immediately before submission; advisory text cannot bypass those checks.

Each contract snapshot includes its floor/cap strike, distance from spot, required move in basis points, seconds remaining, settlement band, realized 30-second move, and the CF Benchmarks BRTI settlement reference. The daemon also applies three-observation hysteresis before changing between `WARMING`, `CONFIRMED_UP`, and `CONFIRMED_DOWN`, preventing a single noisy update from flipping automation state.

Rollout remains staged and visible in the snapshot: shadow observations are always recorded, parallel paper can be enabled independently, and live submission requires an explicitly armed bounded session. Use `kalshi auto decisions` for all current states and `kalshi auto candidates` for hysteretically confirmed states.

`positions` performs authenticated order and fill reconciliation. It binds uncertain submissions by `client_order_id`, deduplicates fills, updates partial/terminal states, and is the only path that books Kalshi fills into local P&L. The daemon runs it every 30 seconds during an armed session.

## Safety behavior

- A local submit timeout becomes `submission_unknown`; the same client ID must be reconciled and must not be resubmitted.
- Filled quantity and fees come from authenticated exchange responses. Create-order acknowledgements do not create P&L entries.
- Filled exposure plus all outstanding remaining quantity consumes the experiment cap.
- New live submissions require the global trading gates, an active bounded session, allowed venue, credentials, fresh executable data, and all deterministic risk checks.
- Cross-exchange spot confirmation requires two live sources, two fresh top-of-book quotes, no more than 12 bps source disagreement, at least 35% directional confidence, and no strong opposing Kalshi contract flow.
- The global kill switch prevents new submissions. Automated micro orders are fill-and-kill; `cancel-orders --yes` is the explicit verified cleanup for any bot-owned order left by another order type or an uncertain network outcome.
- Kalshi REST reads and idempotent writes retry 429 and transient 5xx responses with bounded exponential backoff. Non-idempotent writes are never retried.

Never validate production execution by sending an unbounded or economically meaningful order. Use Kalshi demo first; a live smoke test requires explicit credentials and human arming and is intentionally not part of the automated test suite.

## LLM advisory scoring (`kalshi auto advise`)

### Restart-safe shadow advisor loop

The unattended shadow loop is `scripts/kalshi_advise/advisor_loop.py`. It uses
the existing `open -> commit-blind -> reveal` firewall and writes every selected
opportunity, explicit abstention, malformed response, deterministic gate result,
and shadow proposal to a hash-chained JSONL ledger. It never calls
`prepare_order`, `submit_order`, or any live adapter.

The frozen interfaces are `kalshi-order-proposal-v1`, `kalshi-shadow-gate-v1`,
and `kalshi-qualification-v1`. A future canary executor must consume that exact
proposal/gate output rather than letting a forecaster construct order fields.
Use `advisor_loop.py start|stop|status|report`; `report` combines journal
integrity with the existing resolved `kalshi auto advise score` output and
fails qualification closed until every policy check passes.

The supervisor can be installed as
`advisor_loop.py install --forecaster scripts/kalshi_advise/codex_forecaster.py`.
Its LaunchAgent restarts abnormal exits and preserves heartbeat, pause reason,
safety state, promotion state, canary configuration, and the opportunity hash
chain below the profile daemon directory. The safety actuator is independent of
model output and checks daily realized loss, peak-to-current drawdown,
consecutive losses, open exposure, unknown submissions, and reconciliation age.

Codex qualification epoch v3 (`kalshi-blind-codex-v3-zero-capability`) uses an empty
ephemeral working directory and ignores user config/rules. Its capability policy
pins both the Codex CLI version and the complete feature-registry digest, then
explicitly disables every registered optional feature. Any inventory drift
abstains fail-closed until reviewed and re-pinned; this is a zero-capability
allowlist rather than a list of known-dangerous tools. The read-only sandbox
cannot access absolute files outside the empty workspace.
The production probe is to ask it for a bid from the absolute
`kalshi-ws-books.json` path; any returned price invalidates the epoch. V1 Codex
V1 and v2 rows are never mixed into v3 because `agent_id` contains the prompt/firewall
version and qualification filters by that exact identity.

Promotion states are `SHADOW -> QUALIFIED -> CANARY_ENABLED`, with automatic
`PAUSED`/`DEMOTED` transitions. A failing safety or qualification evaluation
also writes `canary.enabled=false`. Canary v1 hard caps configuration at $2 per
order, $5 aggregate exposure, and $5 daily loss; its pulse delegates only to
the existing deterministic `kalshi auto live execute-next` path after all
advisor gates pass, so it inherits quote freshness, credentials, session,
order-rate, duplicate-contract, and final submission reconciliation checks.
Whole-account daily loss and unresolved settlement accounting remain blockers;
drawdown and consecutive losses replay only exact realized rows after the
canary configuration's immutable `epoch_started_at_ms`.

The Kalshi GUI's **Advisor & Canary** tab is read-only. It consumes the
supervisor's atomic state, safety, promotion, canary, qualification, and
opportunity-journal snapshots; it never recalculates gates or writes those
files. Missing, malformed, stale, or unpinned state is displayed as
`UNKNOWN / FAIL CLOSED`. Both that tab and Auto Cockpit show separate
`LEGACY LIVE SESSION` and `CODEX CANARY` badges so an independently armed
legacy session cannot be mistaken for canary authority. Qualification is
explicitly informational and cannot enable trading from the GUI.

This surface answers one question only: **does an LLM forecaster have predictive skill on Kalshi
crypto settlement, independent of the market and marginal over the deterministic daemon?** It is a
measurement instrument, not a trading path. The daemon retains sole and exclusive ownership of
`execute-next -> prepare_order -> submit_order`; nothing in this surface can create, modify, or
influence an order.

### The six verbs

```
openterminalcli --json --headless kalshi auto advise open --ticker T [--horizon H] [--settlement-def S]
openterminalcli --json --headless kalshi auto advise commit-blind --challenge ID --commit-id K --probability P
openterminalcli --json --headless kalshi auto advise reveal --challenge ID
openterminalcli --json --headless kalshi auto advise commit-post --challenge ID --commit-id K2 --probability P2
openterminalcli --json --headless kalshi auto advise score [--forecaster-id ID | --provider P --model M] [--horizon H]
openterminalcli --json --headless kalshi auto advise ledger [--limit N]
```

The flow is a strict five-state pipeline per challenge:

1. **`open`** snapshots the current contract, computes the horizon-aware TTL, and returns a
   price-free blind `context` plus its `context_hash`. State: `OPENED`.
2. **`commit-blind`** records the forecaster's probability (`p_pre`) against that blind context,
   before anything about the market has been revealed. State: `COMMITTED_BLIND`.
3. **`reveal`** discloses the contemporaneous market quote, the daemon's own probability estimate
   (when one is available), and the sealed hash of the full transcript. State: `REVEALED`.
4. **`commit-post`** records a second probability (`p_post`) now that price is visible — this
   measures whether seeing the market helps or hurts the forecaster. State: `COMMITTED_POST`.
5. **`resolve-by-settlement`** happens automatically as part of ordinary settlement backfill (the
   same widened resolver that resolves deterministic daemon rows also resolves
   `source='llm-advisory'` rows) once the contract settles — there is no separate operator verb for
   it.
6. **`score`** is a read-only report over every resolved, paired row: Brier score against the
   pre-reveal probability, the post-reveal probability, the market, and the daemon; the confidence
   interval on improvement over the daemon; net realized value after Kalshi's taker fee; and a
   coverage breakdown of how many rows were daemon-comparable. `ledger` is the companion read-only
   participation report (opened/committed/revealed/expired counts and rates) — useful for spotting
   selection bias (e.g. a forecaster that commits only when confident, or expires disproportionately
   often on one horizon).

### The firewall guarantee — and its honest limit

`open`'s blind `context` is built by `adv::build_blind_packet()` from a private, per-field
**allowlist**, not a stripped copy: only the handful of fields it names (horizon label, distance,
seconds-left, settlement band/def, spot, realized-move — never a quote or probability field) are
ever copied into the packet, so a new field added anywhere upstream (e.g.
`CryptoMicrostructureRadar` growing a new column) can never leak by default — it simply isn't
copied until someone deliberately allowlists it. `adv::kBlindForbiddenKeys()` is a separate,
independent list — it does not feed `build_blind_packet`'s allowlist at all. It backs a second
layer of defense: the one wholesale sub-object copied into the packet (`spot_microstructure`) is
run through a recursive deep-strip (`kalshi_advise_strip_forbidden_keys_deep`, `CommandDispatch.cpp`)
that removes any of `kBlindForbiddenKeys()`'s names at any nesting depth, and the same list is what
both the e2e smoke and `tst_advisory_protocol`/`tst_command_dispatch` assert never leaked. Two
independent lists catching the same class of leak by construction, rather than one list serving
both jobs.

Withheld until `reveal`: Kalshi `yes/no` bid/ask/depth, `market_implied_probability`, `fair_yes`/
`fair_no`, `divergence`, the daemon's `daemon_probability`/`calibrated_probability`/
`model_probability`/`model_weight`, and any cost/edge figure derived from the quote
(`cost_net_edge`). None of these keys can appear in an `open` response's `context` object under any
circumstances — the e2e smoke (`tests/e2e_headless_smoke.sh`) greps every `advise open` response for
all of them and fails the whole suite if even one appears.

**Honest limit (documented, not enforced):** the protocol firewalls *within a single transaction*
and timestamps commit-before-reveal — `commit-blind` is always written and sealed before `reveal`
can execute, so within this pipeline the forecaster mechanically cannot have seen the withheld data
before committing. What it **cannot** stop is an operator (human or agent) who separately ran
`kalshi auto snapshot`, `edge flow`, or any other price-revealing command moments earlier and then
fed that context into the forecaster out of band. That residual is *logged, not blocked*: every
commit carries forecaster identity (`provider`/`model`/`prompt_version`/`agent_id`/`run_id`) and
precise timestamps, so a suspiciously well-informed pre-reveal commit is visible in the ledger and
in `score`'s output after the fact — this is the tradeoff accepted by choosing an in-process
protocol firewall over a fully separate-credential architecture.

### TTL table (horizon-aware, `adv::ttl_for(seconds_left, configured_max_ms)`)

`configured_max_ms` defaults to 60000 (60 s) and every call site (`advise open`'s handler, the
repository's own re-derivation at commit time) uses that default — there is no separate, larger
absolute-max override anywhere in the current implementation:

| Settlement remaining | Challenge TTL (`prediction_ttl_ms`, before the `configured_max` cap) |
|---|---|
| ≤ 60 s | do not open (`may_open=false`, `prediction_ttl_ms=0`) |
| 1–5 min | 15 s |
| 5–15 min | 30 s |
| 15–60 min | 45 s |
| > 60 min | 60 s |

`prediction_ttl_ms` is `min(bucket_value, configured_max_ms)` — with the 60 s default this only
ever binds on the `>60 min` bucket, which already equals the cap. `execution_relevance_ms` is a
second, independently-derived clock: `min(prediction_ttl_ms / 2, 15000)` — never longer than 15 s
and never longer than half of `prediction_ttl_ms`. A prediction committed after execution
relevance has expired is still journaled and scored — it contributes to `score`'s academic
evidence — but is flagged as never trade-influencing, because by construction nothing downstream
of the daemon acts on advisory output regardless of timing.

### Advisory output can never place an order

Every row this surface writes to `edge_decision_journal` carries `source='llm-advisory'` and three
fixed fields: `"authority": "advisory_only"`, `"execution_eligible": false`, and
`"gate": "measurement_only"`. These are not configuration toggles — there is no flag that promotes
an advisory row to execution authority. The deterministic daemon's own
`execute-next -> prepare_order -> submit_order` path never reads `source='llm-advisory'` rows, and
`kalshi auto advise score`/`ledger` are both read-only reports with no side effect on live trading
state. Any future promotion of LLM output to influence execution (even a bounded weighting or veto)
is out of scope for this feature and would require a separate, explicitly gated, deterministic
design — never an implicit consequence of running `advise`.
