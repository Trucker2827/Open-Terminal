# Expose Coinbase Account State + Gated Execution to the AI/CLI — Design Spec

**Status:** Approved (Approach A; scope = read-only visibility + gated execution; arming = reuse existing arms). **REAL-MONEY capable — live on the user's Coinbase account.**
**Date:** 2026-06-16

## Goal

Give the AI/CLI the same depth of access to the **Coinbase** account (via `ExchangeService`/ccxt) that it has to the Alpaca broker: full read-only **visibility** (balances/holdings, open orders, trade history) plus **gated order execution**, reusing the existing trading-constitution gates. Today only read-only *market-data* crypto tools exist (`get_ticker`/`get_order_book`/`get_candles`/`get_exchange_info`); account state and execution are GUI-only.

## Scope (locked)
- **IN:** read-only crypto account tools; gated `crypto_submit_order` / `crypto_cancel_order`; wiring into the existing gate stack + audit log.
- **OUT:** making Coinbase a first-class `IBroker` (Approach B); multi-account `cli.allowed_account`; per-order GUI confirmation; new arming toggles (we reuse the existing arms by explicit user choice).

## ⚠️ Safety posture (explicit, user-chosen)
Execution gates on the **existing** arms — `cli.allow_trading`, `cli.live_trading_armed`, `cli.fast_live_armed` (all currently **true**), `cli.kill_switch` (false), and `cli_venue_allowed("coinbase")` (coinbase is **already** in `cli.allowed_venues`). **Therefore this feature is LIVE on the real Coinbase account the moment it ships — no further toggle.** The remaining backstops are the deterministic **risk floor** (`cli.risk.max_order_value`, `cli.risk.max_daily_loss`) and the **kill switch**. The AI can never arm itself or raise caps (`is_gui_only_setting` blocks all `cli.*` writes). **Deployment recommendation (not enforced): lower `cli.risk.max_order_value` (currently $500) before first use and test one tiny order.**

## Components

### 1. Read-only account tools (non-destructive, ungated — same posture as `get_ticker`)
Added to `CryptoTradingTools.cpp` (`get_crypto_trading_tools()`), routing to existing `ExchangeService` reads:
- **`get_crypto_balance`** → `ExchangeService::fetch_balance()` → returns the daemon's `{balances:{CUR:{free,used,total}}}` (zero balances already filtered). No params.
- **`get_crypto_open_orders`** → `fetch_open_orders_live(symbol?)`. Optional `symbol`.
- **`get_crypto_trades`** → `fetch_my_trades(symbol, limit=50)`. `symbol` required, optional `limit` (cap 200).

Rationale for ungated reads: the AI runs locally for the user; balances are not a secret from the user's own agent, and visibility is the whole point. Execution — not visibility — is what the constitution gates. (If we later want reads behind `allow_trading`, that's a one-line category change; out of scope now.)

### 2. Gated execution tools (destructive, fast-live-gated)
Added to `CryptoTradingTools.cpp`:
- **`crypto_submit_order`** — params: `symbol`(req), `side`(req buy/sell), `quantity`(req >0), `order_type`(market/limit, default market), `limit_price`(req for limit). Routes to `ExchangeService::place_exchange_order(symbol, side, type, amount, price)`.
- **`crypto_cancel_order`** — params: `order_id`(req), `symbol`(req — ccxt cancel needs it). Routes to `ExchangeService::cancel_exchange_order(order_id, symbol)`.

Both `is_destructive = true`, `auth_required = AuthLevel::Authenticated`.

### 3. Gate wiring (one edit, enforced on all three hosts)
Add `crypto_submit_order` and `crypto_cancel_order` to the canonical-name list in **`SettingsGate.cpp::is_fast_live_tool()`** (currently `{fast_submit_order, cancel_order, replace_order, exit_position, get_positions, get_open_orders, get_fills}`). The three AI-facing auth-checkers (`ServeCommand.cpp`, `HeadlessRuntime.cpp`, `AgentService.cpp`) already route an `is_fast_live_tool` match through `cli_trading_allowed() && cli_live_armed() && cli_fast_live_armed()`. They are **not** added to `is_live_execution_tool` (that predicate stays the raw `live_*` deny-list) — so they get the gated carve-out status, identical to `fast_submit_order`.

### 4. In-handler gate sequence (defense-in-depth — handler re-enforces, never trusts the router)
`crypto_submit_order` runs, in order, before any exchange call:
1. **Kill switch:** `cli_kill_switch_engaged()` → refuse + audit `decision="denied"`, `reason="kill switch engaged"`.
2. **Arms:** `cli_trading_allowed() && cli_live_armed() && cli_fast_live_armed()` → else refuse + audit `denied`.
3. **Venue:** `cli_venue_allowed(ExchangeService::instance().get_exchange())` (e.g. "coinbase") → else refuse + audit `denied` `reason="venue not allowed"`.
4. **Risk floor** (`crypto_risk_floor`, §5) → on breach refuse + audit `denied` with the cap in `reason`.
5. **Execute:** `place_exchange_order(...)`.
6. **Audit:** record the broker result (§6).

`crypto_cancel_order` runs steps 1–3 then cancels (no risk floor on a cancel) then audits.

### 5. Risk floor for crypto (`crypto_risk_floor`)
Mirrors `FastLiveTools::fast_risk_floor` and reads the **same** cap keys so behaviour is consistent with the equity rail:
- `max_order_value = cli.risk.max_order_value` (same default/read path).
- **Resolved price:** `limit` order → `limit_price`; `market` order → `ExchangeService::get_cached_price(symbol).last`, falling back to `fetch_ticker(symbol).last` if the cache is cold. If still 0 → refuse (`reason="no price available"`).
- `order_value = quantity * resolved_price` (contract multiplier = **1** for crypto — `option_contract_multiplier` is irrelevant; spot only).
- Reject if `order_value > max_order_value`.
- Daily-loss: feed `max_loss = order_value` into the same daily-loss check the fast rail uses against `cli.risk.max_daily_loss`.
- To avoid touching the working fast path (its helper is anonymous-namespace/local), `crypto_risk_floor` is a small dedicated function in `CryptoTradingTools.cpp` reading the identical setting keys. Documented as a deliberate, narrow mirror (not a copy of unrelated logic).

### 6. Audit logging
Every execution attempt (allowed or denied) records a `TradeAuditRow` via `TradeAuditRepository::append()`:
`phase="execute"`, `tool="crypto_submit_order"|"crypto_cancel_order"`, `account=ExchangeService::get_exchange()` (e.g. "coinbase"), `mode="live"`, `intent_json` (the order params), `decision` (broker status: `new`/`accepted`/`filled`/`cancelled`/`rejected`, or `denied` for gate refusals), `reason`, `risk_snapshot_json` (resolved price, order_value, caps). This flows through the existing `trade.audit` EventBus emit → AI-activity toasts + Settings→AI Activity log, so the human sees every real Coinbase order.

## Data flow
```
AI/CLI: get_crypto_balance {}              → ExchangeService.fetch_balance → {balances:{USD,BTC,...}}   (visibility)
AI/CLI: crypto_submit_order {symbol:BTC/USD, side:buy, quantity:0.001, order_type:limit, limit_price:60000}
   → [auth-checker] is_fast_live_tool → allow_trading && live_armed && fast_live_armed
   → [handler] kill-switch → arms → venue(coinbase) → crypto_risk_floor (0.001 × 60000 = $60 vs $500 cap)
   → ExchangeService.place_exchange_order(BTC/USD, buy, limit, 0.001, 60000)
   → trade_audit(execute, crypto_submit_order, coinbase, live, …, new) → toast + AI-activity row
```

## Error handling
- Daemon/exchange errors (`{"error":...}` or `success:false`) → tool returns a clean structured error + audit `decision="rejected"`, `reason=<exchange message>`; never crashes (same swallow-proofing as the balance path fix).
- Gate refusals are recorded, not silent.
- Missing/zero price on a market order → refuse via the risk floor (no blind market order).
- `cancel` of an unknown order id → exchange error surfaced verbatim.

## Testing
- **Unit (pure/gate):** `crypto_risk_floor` matrix — under-cap passes, over-cap rejected; market-order price resolution (cached → fetch → 0=refuse). Neuter-proofed (throwing checks, not `assert`).
- **Unit (gate routing):** `is_fast_live_tool("crypto_submit_order")` and `("crypto_cancel_order")` are **true**; `is_live_execution_tool(...)` for them is **false**. Assert in the existing SettingsGate test suite.
- **Unit (kill/arm/venue refusal):** with kill switch on → refuse; with an arm off → refuse; with `cli.allowed_venues` lacking coinbase → refuse. Each records a `denied` audit row.
- **Integration (manual, live paper-equivalent):** there is no Coinbase sandbox in ccxt for CDP; verify against the user's live account with a **far-from-market limit + immediate cancel** at a **tiny** size under a lowered `max_order_value`, then confirm via `get_crypto_open_orders` + the audit row. Read-only tools verified live (balance shows the real currencies).

## Files
- `src/mcp/tools/CryptoTradingTools.{cpp,h}` — add 3 read tools + 2 exec tools + `crypto_risk_floor`.
- `src/mcp/tools/SettingsGate.cpp` — add the 2 exec tool names to `is_fast_live_tool()`.
- `src/storage/repositories/TradeAuditRepository` — reused as-is (append).
- `tests/tst_settings_gate.cpp` (or the gate test) — routing asserts; a `crypto_risk_floor` unit test (new `tests/tst_crypto_risk.cpp` or fold in).
- No change to the Alpaca/broker path, `AccountManager`, or `IBroker`.

## Risks
- **Real money, live-on-ship:** the headline risk. Mitigations: risk floor + daily-loss cap + kill switch + audit/toasts; deployment note to lower the cap and test tiny. The arming choice is the user's, documented here.
- **Mis-gating (tool reachable without arms):** mitigated by adding to `is_fast_live_tool` (router) AND re-checking in-handler (defense-in-depth) + the routing unit test.
- **Price resolution wrong → oversized order:** the floor refuses on a zero/again-zero price; limit orders carry their own price.
- **Venue drift:** the handler reads the live `ExchangeService::get_exchange()` so the venue check reflects the actually-connected exchange, not a hardcoded "coinbase".

## Follow-ups (out of scope)
- `replace_order` / `exit_position` equivalents for crypto.
- A dedicated per-venue risk cap (separate from the shared `cli.risk.*`).
- Reduce-only / stop / stop-limit crypto orders (the adapter supports them; v1 is market/limit).
- Optional `allow_trading`-gating of the read tools if balance privacy is later desired.
