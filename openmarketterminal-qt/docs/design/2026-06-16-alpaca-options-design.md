# Wire Alpaca Options (v1) — Design Spec

**Status:** Approved (design + scope locked: AI-tool path). **Real-money-adjacent — sandbox-validated.**
**Date:** 2026-06-16

## Goal

Make Alpaca options usable from the AI/CLI trading path, safely. Empirically verified: single-leg option **orders already flow through the existing path** — `fast_submit_order` with an OCC symbol (e.g. `AAPL260821C00110000`) reached Alpaca and was accepted as `asset_class: us_option`, then canceled. So order plumbing works via the adapter's symbol pass-through. This feature closes the three gaps that block real, safe options use — **without a new GUI** (the human/AI discovers a contract via a tool, then trades it with the existing order flow).

## Scope (locked)
- **IN:** the 100× risk-floor safety fix (mandatory), a contract-discovery tool, and a validator tweak.
- **OUT (v1):** GUI options chain, Greeks, option quotes in `get_quote`, multi-leg/spreads, exercise/assignment.

## Components

### 1. Safety fix — option contract multiplier (MANDATORY, the critical piece)
An option contract represents **100 shares**, but both risk floors compute `order_value = quantity × price` (so a $2.00 option reads as $2 of exposure when it's actually $200/contract). That makes `cli.risk.max_order_value` and the daily-loss cap **100× too loose** on options.

**Fix:** a pure, core helper:
```cpp
// src/trading/options/OptionSymbol.{h,cpp}  (namespace openmarketterminal::trading)
bool is_occ_option_symbol(const QString& symbol);   // OCC format match
int  option_contract_multiplier(const QString& symbol); // 100 if OCC option, else 1
```
- OCC detection: matches `^[A-Z]{1,6}[0-9]{6}[CP][0-9]{8}$` (root 1-6 upper-alpha + YYMMDD + C/P + 8-digit strike×1000). Equity (`AAPL`), crypto (`BTC/USD`, has `/`), and `EXCH:SYM:CONID` (has `:`) never match. Malformed → `1`.
- Applied in **both** floors:
  - `FastLiveTools.cpp:233` `fast_risk_floor`: `rv.order_value = o.quantity * resolved_price * trading::option_contract_multiplier(o.symbol);`
  - `OrderFlowTools.cpp:112` `risk_floor_check`: same.
  - `rv.max_loss = rv.order_value;` (unchanged line) then carries the ×100 exposure into the daily-loss check automatically.
- `max_position_qty` stays raw contract count (left as-is): `order_value` is the binding constraint for options; a contract-count cap of 10000 is irrelevant next to the notional cap. Documented, not changed.

### 2. Discovery — `get_option_contracts` tool (read-only)
A new **non-destructive** MCP tool (reachable like `get_quote` — no destructive token), so the AI/human can find an OCC symbol to trade.
- Params: `underlying` (required, e.g. "AAPL"); optional `type` ("call"|"put"), `expiry_gte`/`expiry_lte` (YYYY-MM-DD), `strike_gte`/`strike_lte` (number), `limit` (default 50, cap 200).
- Routes to the **allowed account's broker** (`cli.allowed_account` → `AccountManager::broker_for`), then a new adapter method:
  ```cpp
  // AlpacaBroker
  ApiResponse<QJsonArray> get_option_contracts(const BrokerCredentials& creds, const QJsonObject& params);
  ```
  which GETs `/v2/options/contracts` (with `underlying_symbols`, `status=active`, `expiration_date_gte/lte`, `type`, `strike_gte/lte`, `limit`) and returns the `option_contracts` array.
- Tool output: a list of `{ symbol (OCC), underlying, expiration, type, strike, close_price, tradable }` (mapped from Alpaca's fields; tolerate missing fields). On no allowed account / non-Alpaca broker / API error → a clean structured error (not a crash).
- Gating: read-only + account-scoped, same posture as `get_quote`/`get_positions` reads — no order gates, no destructive token. (It only reads contract metadata.)

### 3. Validator accepts options
Add `OPRA` to `OrderValidator::valid_exchanges()` (`OrderValidator.cpp:6`). The omit-exchange→broker-default path already works (bug #3 fix); this just makes an explicit `OPRA` valid too, so an AI that passes the options exchange isn't rejected.

### The order path itself
Unchanged — `fast_submit_order`/`submit_order` already accept an OCC symbol and route it to Alpaca (verified live). With the multiplier fix, the risk floor now sizes options correctly before firing.

## Data flow
```
AI/CLI: get_option_contracts {underlying:AAPL, type:call, expiry_gte:2026-08-01}
   → allowed account's AlpacaBroker.get_option_contracts → /v2/options/contracts
   → [{occ_symbol, strike, expiration, close_price}, ...]
AI/CLI: fast_submit_order {symbol:<occ_symbol>, side:buy, quantity:1, order_type:limit, limit_price:X}
   → gate stack → fast_risk_floor: order_value = 1 × X × 100  (CORRECT exposure)
   → Alpaca us_option order → toast + AI-activity row (existing)
```

## Error handling
- `option_contract_multiplier` never throws; non-OCC → 1 (equities/crypto unaffected — regression-safe).
- `get_option_contracts`: no allowed account → "no allowed account configured"; broker isn't Alpaca / lacks the method → "options discovery not supported for this broker"; HTTP error → the broker's message; empty result → empty list (success). Never crashes.
- Risk floor with a missing option price falls through the existing "no price available" path (limit orders carry their own price).

## Testing
- **Pure (core, unit):** `option_contract_multiplier` — matrix: OCC calls/puts across roots/strikes (`AAPL260821C00110000`, `SPY261218P00450000`, `F270115C00012500`) → 100; `AAPL`, `BRK.B`→(no digits)→1, `BTC/USD`→1, `NASDAQ:AAPL:265598`→1, `""`→1, malformed (`AAPL26X`...)→1. Neuter-proofed.
- **Risk floor (unit, the safety bite):** in `tst_fast_live` (+ mirror in `tst_order_flow` if cheap): an option order whose `qty × price` is **within** `max_order_value` but `× 100` **exceeds** it → now **rejected** ("exceeds max order value"); the same notional as a plain equity symbol still passes. Proves the multiplier is load-bearing.
- **Discovery tool:** integration-verified live against Alpaca (like `get_quote`); a unit test of the contract-array mapping if a pure mapper is extracted. Headless test can't reach Alpaca, so the live call is manual/best-effort.
- **Validator:** `is_valid_exchange("OPRA")` true.

## Files
- `src/trading/options/OptionSymbol.{h,cpp}` — pure helper (core; tested).
- `src/mcp/tools/FastLiveTools.cpp` + `src/mcp/tools/OrderFlowTools.cpp` — apply the multiplier in the two risk floors.
- `src/trading/brokers/alpaca/AlpacaBroker.{h,cpp}` — `get_option_contracts`.
- a new `get_option_contracts` MCP tool (new `OptionsTools.{cpp,h}` or extend `MarketsTools`) + registration in the core tool init.
- `src/trading/OrderValidator.cpp` — add `OPRA`.
- `tests/tst_fast_live.cpp` (+ a new `tests/tst_option_symbol.cpp` for the pure helper, or fold into an existing suite) + `tests/CMakeLists.txt`.

## Risks
- **Mis-detecting an equity as an option (false 100×)** → would over-reject legit equity orders. Mitigation: the OCC regex requires the 6-digit date + C/P + 8-digit strike (15+ chars); no real equity/crypto symbol matches; covered by the test matrix.
- **Missing an option (false ×1)** → the original safety hole persists for that symbol. Mitigation: the regex matches the canonical OCC format Alpaca returns; `get_option_contracts` returns exactly those symbols.
- Options are real-money-capable on a live account — same constitution (arms/kill/allowed-account/floor) applies; this feature only makes the floor size them correctly. Sandbox-validated on the Alpaca paper account.

## Follow-ups (out of scope)
- GUI options chain (browse strikes/expiries, click-to-trade).
- Option quotes/Greeks (a real-time chain) + `get_quote` support for OCC symbols.
- Multi-leg / spreads (Level-3 mleg orders — different Alpaca schema).
- Contract-multiplier from the contract metadata (rare non-100 multipliers, e.g. adjusted contracts) instead of the fixed 100.
