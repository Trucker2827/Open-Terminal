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
