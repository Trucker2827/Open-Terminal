# Crypto Window Improvements (Coinbase-first) — Design Spec

**Date:** 2026-07-21
**Status:** Draft — pending user review, then implementation plan
**Scope:** The crypto trading screen (`src/screens/crypto_trading/`) and its
supporting services only. Four phases: (1) Coinbase-correctness fixes,
(2) authenticated account WebSocket + live ladder overlay, (3) unify crypto
paper trading onto the sandbox substrate, (4) alerts. The Markets screen is
explicitly out of scope. The crypto autonomous-advisor lane is explicitly
out of scope (post-duel).

---

## ⚠️ Deployment constraint (binding for every phase)

The Claude-vs-Codex blind forecasting duel is LIVE. Its serve daemon and the
advisor supervisor run from **this same binary/repo substrate**
(`org.openterminal.kalshi-advisor.default` launchd job + the `serve` daemon).

- **Branch work, builds, and tests are unrestricted.**
- **Installing a new app binary, restarting the serve daemon, or touching any
  launchd job is FORBIDDEN until a duel-safe window** (epoch milestone or an
  explicit user go-ahead). Each phase's PR merges when green; *deployment* is
  batched and scheduled separately.
- No phase below may modify `cli/ServeCommand.cpp`,
  `scripts/kalshi_advise/*`, or anything under the advisor state directory.
  One deliberate, isolated exception is called out in P1-3 (a legacy endpoint
  in `KalshiScreen.cpp`) with its own guard rail.

---

## Goal

The crypto screen is already a competent manual terminal (chart, order book,
DOM ladder + VAP, depth, time & sales, order entry with SL/TP, paper/live
blotter, WS public-data streaming with REST fallback, Coinbase Advanced CDP
auth working end-to-end). What it lacks:

1. **Coinbase-native correctness** — the symbol universe is hardcoded USDT
   while the logged-in venue trades USD/USDC; status chrome lies; deprecated
   Coinbase Pro endpoints linger in supporting services.
2. **Real-time account state** — balances/positions/orders/fills are
   REST-polled on timers; fills appear only on the next poll tick.
3. **Own-orders on the live ladder** — the marquee DOM feature is unwired in
   Live mode.
4. **Honest paper trading** — crypto paper mode bypasses the freshness-gated
   sandbox substrate the Kalshi side proved out.
5. **Alerts** — zero wiring to the app's notification system.

## Non-goals

- **No autonomous crypto trading, no advisor lane, no LLM anywhere.** The
  future "crypto advisor on the Kalshi chassis" idea is deliberately deferred
  until the duel epoch resolves; nothing in this spec builds toward it except
  incidentally (P3's honest paper substrate).
- No Markets-screen changes (`src/screens/markets/`, `MarketDataService`).
- No new venues; no changes to Kraken-native or Hyperliquid-native paths
  beyond what P1-1's per-exchange symbol table forces mechanically.
- No changes to the gated `crypto_submit_order`/`crypto_cancel_order` MCP
  execution path or its risk gates (working, live-verified; leave alone).
- No click-to-trade changes on the ladder (overlay is display-only, matching
  the DOM-ladder Phase-1 spec).

---

# Phase 1 — Coinbase-correctness fixes

## P1-1: Per-exchange symbol universe (kill the USDT remap)

**Problem.** `CryptoTradingScreen.h:191-196` hardcodes ~25 `*/USDT` watchlist
pairs and `default_symbol()` (`CryptoTradingScreen.cpp:378`) returns
`BTC/USDT` in multi-asset mode. Coinbase Advanced trades USD (and USDC)
quotes for most products. Today this is papered over with ~11 on-the-wire
remaps (display says `/USDT`, wire says `/USD`), which also causes the known
"AVAIL shows USDT but the value is USD" label bug (the unit comes from the
displayed pair quote, not the matched balance currency).

**Design.**

- New pure helper (unit-testable, no Qt widgets):
  `src/screens/crypto_trading/CryptoSymbolUniverse.h` —
  `quote_currency_for(exchange_id)` and
  `default_watchlist_for(exchange_id)`. Coinbase → `/USD`; existing
  USDT-quoted venues (binance, okx, bybit, kucoin, bitget, gate, mexc) keep
  `/USDT`; kraken → `/USD` (verify against the native client's current pair
  list); hyperliquid unchanged (perp naming is its own scheme).
- `CryptoTradingScreen` consumes the helper on exchange change: watchlist
  contents, `default_symbol()`, and the restore-state fallback all become
  exchange-aware. The display pair IS the wire pair — delete the remap layer
  entirely (grep for the remap table; it must be gone, not bypassed).
- **AVAIL label fix falls out:** the available-balance unit label is driven by
  the matched balance currency from the fallback chain already implemented in
  `async_fetch_live_balance` (pair-quote → USD → USDC → USDT → largest), not
  by string-splitting the displayed pair.
- Saved user state migration: on first launch after upgrade, if the persisted
  symbol for a `/USD`-quoted exchange is a `/USDT` pair, map it through the
  same helper and persist back. Never silently discard a user's watchlist
  additions — remap only the quote suffix, leave unknown pairs untouched.

**Tests.** Unit tests on `CryptoSymbolUniverse` (coinbase→USD, binance→USDT,
migration mapping, unknown-pair passthrough). Regression: a test that fails
if any remap-table symbol survives (grep-style assertion in a unit test on
the deleted table's absence is brittle — instead assert
`default_symbol("coinbase") == "BTC/USD"` and that the watchlist for
coinbase contains no `/USDT` entry; both fail on today's code).

## P1-2: Honest status chrome (API + DAEMON indicators)

**Problem.** The "API" button and "DAEMON" label in the crypto status bar are
static chrome — never updated — so a fully working connection reads as
broken/greyed. The only live indicator today is the WS pill.

**Design.**

- **API indicator** = credentialed state: green when `ExchangeSession` has
  stored credentials for the active exchange AND the last authenticated fetch
  (balance) succeeded; amber when credentialed but last fetch errored; grey
  when no credentials. Driven by the existing async-fetch completion paths in
  `CryptoTradingScreen_AsyncFetch.cpp` — no new requests, no polling added.
- **DAEMON indicator** = `ExchangeDaemonPool` subprocess liveness for the
  active exchange (green = daemon process alive and last RPC round-trip OK,
  red = spawn failure/dead pipe). The pool already knows this; surface it via
  a signal rather than the screen asking.
- Tooltips state the exact meaning ("API: credentials stored, last
  authenticated call OK at HH:MM:SS") — the indicator must never claim more
  than what was actually observed.

**Tests.** Widget-level test with a stubbed session/pool driving all state
transitions (no-creds → credentialed-ok → fetch-error → daemon-dead).
Neuter check: force the stub to error and confirm the indicator leaves green.

## P1-3: Migrate sunset Coinbase Exchange (Pro) endpoints

**Problem.** Three call sites still use the deprecated Coinbase Pro /
Exchange API, distinct from the ccxt Advanced Trade path the trading screen
uses. When Coinbase turns these off, the features die silently:

- `cli/CommandDispatch.cpp:20244` — `https://api.exchange.coinbase.com/products/%1/ticker`
- `cli/CommandDispatch.cpp:20533` — same host, `/candles`
- `services/crypto_latency/CryptoLatencyService.cpp:325` — `wss://ws-feed.exchange.coinbase.com`
- `screens/kalshi/KalshiScreen.cpp:3804` — same ws-feed (spot reference price)

**Design.**

- REST ticker/candles → Coinbase Advanced Trade public market-data endpoints
  (`api.coinbase.com/api/v3/brokerage/market/products/...` — public, no auth
  needed for ticker/candles). Same JSON-shape adaptation in place; no new
  service.
- WS feed → Advanced Trade WebSocket (`advanced-trade-ws.coinbase.com`,
  public `ticker` channel, no auth for market data).
- **Guard rail for `KalshiScreen.cpp:3804`:** this file feeds the prediction
  side. The change is confined to the URL + message-parse for the spot
  reference; PR must diff ONLY that block, and the PR description must state
  the duel-deployment constraint (merged ≠ deployed). If the parse shape
  can't be verified live without deploying, ship behind the old-endpoint
  fallback: try Advanced WS, fall back to the legacy feed while it still
  answers, log which path served.

**Tests.** Parse-layer unit tests against captured real payloads from the new
endpoints (capture with a throwaway script during development — read-only,
public, does not touch the app or daemons). Regression: old-URL absence
asserted per file (a unit test on a small `coinbase_endpoints.h` constants
header that all four call sites must consume — hardcoded URLs elsewhere
become grep-visible review items).

---

# Phase 2 — Real-time account data + live ladder overlay

## P2-1: Coinbase authenticated user-channel WebSocket

**Problem.** Account data (balance, positions, open orders, my-trades) is
REST-polled on `live_data_timer_` (`CryptoTradingScreen.h:110-115`). Fills
and cancels appear only on the next poll. The app already has the right
pattern — `trading/AccountDataStream.{h,cpp}` (per-account WS + cached
positions/orders/funds) — but it's wired only to equity brokers.

**Design.**

- Extend the **ccxt daemon path, not C++**: `scripts/exchange/ws_stream.py`
  already runs ccxt.pro per-symbol public streams. Add an authenticated
  sibling mode (`--account`) using ccxt.pro's `watchOrders` /
  `watchMyTrades` / `watchBalance` for the active exchange, publishing to new
  DataHub topics `ws:<exchange>:orders:*`, `ws:<exchange>:mytrades:*`,
  `ws:<exchange>:balance`. Credentials reach the subprocess the same way the
  REST daemon gets them today (existing `set_credentials` RPC path — never
  argv, never env-with-secret-in-`ps`).
- ccxt.pro normalizes the Coinbase user channel, so the C++ side receives the
  same unified order/trade shapes for every daemon exchange — this is what
  makes P2-2 cheap, and it works for Kraken/Binance/etc. for free.
- `CryptoTradingScreen` subscribes; on any account-WS event, updates the
  blotter immediately AND schedules one confirming REST fetch (WS is a
  latency optimization, REST remains the source of truth — same
  belt-and-suspenders the Kalshi reconciler uses).
- **Fallback:** if the account WS is unavailable (venue, creds, subprocess
  death), the existing timer polling continues unchanged. The feature is
  additive; degraded mode == today's behavior, indicated on the DAEMON/WS
  chrome from P1-2.
- Poll cadence relaxes (e.g. 5s → 30s) ONLY while the account WS is
  confirmed live within the last N seconds; any staleness snaps polling back
  to today's cadence. Fail toward more polling, never less.

**Tests.** Python-side: unit test the new ws_stream account mode against
recorded ccxt.pro message fixtures (orders/mytrades/balance), including the
credential-injection path. C++-side: stub DataHub publications and assert
blotter update + confirming-fetch scheduling + cadence snap-back on
staleness. Neuter: drop the WS-liveness flag and confirm cadence returns to
baseline.

## P2-2: Live-mode ladder overlay (own orders + average entry)

**Problem.** `update_ladder_overlay` works only in Paper mode;
`CryptoTradingScreen.h:243-248` documents that Live was left unwired because
live orders arrive as raw per-exchange JSON. The DOM ladder spec
(2026-07-08) always intended the ORDERS column + avg-entry marker.

**Design.**

- Consume the **normalized** unified-order stream from P2-1 (this is why
  P2-2 depends on P2-1): map open orders for the active symbol to
  `{price, side, qty}` overlay entries; map position/fills to average entry.
- Spot venues (Coinbase) have no `fetchPositions` — average entry is
  computed from my-trades for the active symbol (VWAP of the net open
  quantity), clearly labeled "est. avg entry" because deposit/withdrawal
  history can make it approximate. If net quantity ≤ 0, no marker.
- Display-only. The click seam stays unwired (per the DOM-ladder spec's
  standing non-goal).

**Tests.** Extend the existing `CryptoLadderModel` unit tests: overlay
bucketing of live orders at grouping boundaries, est-avg-entry VWAP math
(buys/sells interleaved, zero-net case → no marker), symbol-switch clears
overlay. Neuter each.

---

# Phase 3 — Unify crypto paper trading onto the sandbox substrate

**Problem.** Crypto Paper mode is a separate SQLite engine
(`trading::PtPortfolio`, `PaperTrading.cpp`; screen creates "Crypto Paper"
portfolio at `CryptoTradingScreen.cpp:682-688`). The app's proven substrate —
`services/sandbox/` `PaperExecutor` / `PaperFillModel` / `FreshnessGate` /
`SandboxRegistry` / `Scorer` — is unused by crypto (zero references). So
crypto paper fills are not freshness-gated and not walk-the-book honest,
while Kalshi paper fills are.

**Design.**

- Route crypto Paper order submission through `PaperExecutor` with a crypto
  fill model: walk the live order-book snapshot at submit time (the book is
  already streaming in-screen), partial fills on thin depth, taker fees from
  the existing fee settings, and `FreshnessGate` refusal when the book is
  stale (>5s or WS down → the order is REJECTED with a visible reason, not
  filled against a dead book — this is the honesty upgrade).
- Blotter reads fills/positions from the sandbox registry for Paper mode;
  Live mode untouched.
- **Migration:** existing `PtPortfolio` "Crypto Paper" history is preserved
  read-only (old fills remain viewable); new paper fills go to the sandbox
  path. No silent balance carry-over — the paper portfolio restarts at a
  configured bankroll, stated in the UI ("Paper bankroll reset on upgrade to
  freshness-gated fills, previous history archived").
- This phase intentionally makes crypto paper results comparable in rigor to
  Kalshi paper results — the prerequisite for ANY future strategy
  measurement on this screen — without building any strategy/advisor code.

**Tests.** Fill-model unit tests (walk-the-book partial fill, fee
application, stale-book rejection — each neutered). Screen-level: paper
submit during forced-stale book → visible rejection, no fill row. Migration
test: old portfolio present → archived + readable, new fills land in
sandbox.

**Open question for review:** keep `PtPortfolio` for other screens (it may
have non-crypto users — audit first) or deprecate it entirely. The audit is
part of this phase's plan, not assumed here.

---

# Phase 4 — Alerts

**Problem.** `screens/crypto_trading/` references none of
`services/notifications` / `TradingNotificationBridge`. No price alerts, no
fill notifications.

**Design.**

- **Fill/cancel notifications (free with P2-1):** account-WS order events →
  `TradingNotificationBridge` ("Coinbase: BUY 0.001 BTC/USD filled @
  118,250"). Live mode only; paper fills stay silent by default (toggle).
- **Price-cross alerts:** right-click a watchlist row or the chart → "Alert
  at price…"; stored via `SettingsRepository` per exchange+symbol; evaluated
  against the already-streaming ticker in the screen (no new feed, no daemon
  involvement); fires once then disarms (re-armable). Persisted across
  restart.
- **Spread alert (cheap, optional in plan):** alert when top-of-book spread
  exceeds N bps for the active symbol — useful because wide spread is
  exactly when manual trading gets expensive.
- All alerts are LOCAL app notifications only. No external delivery, nothing
  touches daemons or launchd.

**Tests.** Pure alert-evaluator unit tests (cross-up, cross-down, disarm
after fire, re-arm, persistence round-trip). Bridge-level test that a
synthetic order event produces exactly one notification (dedupe by order id
— WS + confirming REST fetch must not double-notify; neuter the dedupe and
confirm the test fails).

---

## Sequencing & dependencies

```
P1-1 (symbols)  P1-2 (chrome)  P1-3 (endpoints)   — independent, any order, small
        →  P2-1 (account WS)  →  P2-2 (ladder overlay)
                              →  P4 fill notifications
        →  P3 (sandbox paper)                       — independent of P2
        →  P4 price/spread alerts                   — independent of P2
```

Each phase = its own branch + PR, Codex implements, Claude reviews (same flow
as the Kalshi work). Every fix ships with a regression test that fails
without it (neuter-verified); throwing/exit-nonzero checks, no bare
`assert()` in gating tests. **All merged PRs wait for a duel-safe window to
deploy.**

## Explicitly deferred (do not build now)

- Crypto advisor lane on the Kalshi chassis (blind forecaster → deterministic
  gate → shadow). Revisit after the Claude-vs-Codex epoch resolves.
- Ladder click-to-trade.
- Any Markets-screen work.
- Per-venue fee-tier awareness (pulling the account's actual Coinbase fee
  tier via ccxt `fetchTradingFees` is a nice-to-have; current static fee
  settings are acceptable).
