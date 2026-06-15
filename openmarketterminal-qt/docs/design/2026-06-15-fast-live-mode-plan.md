# Fast Live Mode (Phase D) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. **REAL-MONEY phase — build + test ONLY against a FakeBroker / sandbox account; never real credentials.** Every fast-live tool is a GATED WRAPPER (kill-switch → fast-armed → allowed-account → [risk floor] → broker op → audit) over `UnifiedTrading` account-aware ops; the raw `live_*` tools stay denied on AI hosts.

**Goal:** a human-armed **fast live mode** (`cli.fast_live_armed`, GUI-only) that unlocks a low-latency live action set for the AI: `fast_submit_order` (one-shot), `cancel_order`, `replace_order`, `exit_position`, `get_positions`, `get_open_orders`, `get_fills` — all gated by the constitution, which the AI cannot change.

**Architecture:** gated MCP wrappers in a new `src/mcp/tools/FastLiveTools.{h,cpp}`, routed ONLY to `cli.allowed_account`, reusing the existing `UnifiedTrading` broker ops + the Phase-C live-P&L ledger + `trade_audit`. A new `cli.fast_live_armed` constitution flag + an `is_fast_live_tool` classifier gate the set in all three host checkers (ServeCommand/HeadlessRuntime/AgentService); raw `live_*` denial unchanged.

**Tech Stack:** C++20, Qt6, CMake+Ninja, QtTest.

**Spec:** `docs/design/2026-06-15-fast-live-mode-design.md`.
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · test build `/tmp/ot-build-test` · GUI build `/tmp/ot-build-ht` · **Branch:** `feat/fast-live-mode` off `main`.

---

## Verified facts (grounded)
- **Broker ops** (`UnifiedTrading.h`, `trading::UnifiedTrading::instance()`): `UnifiedOrderResponse place_order(account_id, UnifiedOrder)` (→ fast_submit), `UnifiedOrderResponse cancel_order(account_id, order_id)` (→ cancel_order), `UnifiedOrderResponse modify_order(account_id, order_id, QJsonObject mods)` (→ replace_order), `ApiResponse<OrderPlaceResponse> close_position(account_id, symbol, exchange, product)` (→ exit_position). Live reads: `AccountManager::instance().broker_for(account_id)` → `IBroker*`, `load_credentials(account_id)`, then `broker->get_positions(creds)` / `get_orders(creds)` (see `LiveTradingTools.cpp:446-540` for the exact shapes + result types `BrokerPosition`/`BrokerOrderInfo`). Reuse the OPS; do NOT expose the raw `live_*` tools.
- **Constitution helpers** (`SettingsGate.{h,cpp}`, ns `openmarketterminal::mcp`): `cli_trading_allowed()`, `cli_live_armed()`, `cli_kill_switch_engaged()`, `cli_allowed_account()`, `is_gui_only_setting()` (the `cli.` prefix → `cli.fast_live_armed` is GUI-only for free), `is_live_execution_tool()` (raw `live_*` denial). Add `cli_fast_live_armed()` + `is_fast_live_tool()`.
- **Risk floor + ledger** (`OrderFlowTools.cpp` + `LivePnl.{h,cpp}`): `risk_floor_check`/`read_cap`, `mcp::tools::daily_loss_ok(maxloss)`, `record_open`/`record_close`. The fast-submit/replace reuse the risk floor; fills update the live ledger.
- **Three host checkers:** daemon `ServeCommand.cpp`, headless `HeadlessRuntime.cpp`, GUI `AgentService.cpp` — each has the `submit_order` carve-out + `is_live_execution_tool` denial. Add an `is_fast_live_tool` branch: allow iff `cli_trading_allowed() && cli_live_armed() && cli_fast_live_armed()`; else deny. (Handlers re-enforce the full stack.)
- **`UnifiedOrder`** (TradingTypes.h): symbol/exchange/side(OrderSide)/order_type(OrderType)/quantity/price/... `ToolResult ok_data/fail`. `audit_submit` is in OrderFlowTools (or add a small `fast_audit` helper writing `trade_audit` with tool+mode "live"+account).
- **Test seam:** `FakeBroker : trading::IBroker` (from `tst_live_trading.cpp`, Phase C) + `BrokerRegistry::register_broker_for_test` + a sandbox-mode account. Reuse it.
- Migrations: NONE (reuse `trade_audit` + v051 ledger).

---

## File Structure
- Create: `src/mcp/tools/FastLiveTools.{h,cpp}` (the 7 gated tools + a shared `fast_live_gate(reason_out)` helper); 
- Modify: `src/mcp/tools/SettingsGate.{h,cpp}` (`cli_fast_live_armed`, `is_fast_live_tool`), `src/mcp/McpInit.cpp` (register), `src/cli/ServeCommand.cpp` + `src/core/headless/HeadlessRuntime.cpp` + `src/services/agents/AgentService.cpp` (the `is_fast_live_tool` carve-out), `src/screens/settings/SecuritySection.{h,cpp}` (the toggle), `CMakeLists.txt`, `tests/CMakeLists.txt`.
- Tests: `tests/tst_fast_live.cpp`; `tests/e2e_fast_live.sh`.

---

## Task 1: Constitution + gating (cli.fast_live_armed) + the shared gate

**Files:** Modify `SettingsGate.{h,cpp}`, `ServeCommand.cpp`, `HeadlessRuntime.cpp`, `AgentService.cpp`, `SecuritySection.{h,cpp}`; Test `tests/tst_fast_live.cpp`.

- [ ] **Step 1: SettingsGate** — `bool cli_fast_live_armed()` → `flag_true("cli.fast_live_armed")` (default false). `bool is_fast_live_tool(const QString& name)` → true for exactly `{fast_submit_order, cancel_order, replace_order, exit_position, get_positions, get_open_orders, get_fills}` (a static set; resolve canonical name like `is_settings_write_tool` if aliases matter). Declare both in `SettingsGate.h`.
- [ ] **Step 2: Host carve-outs** — in ALL THREE checkers, AFTER the `submit_order` branch and the `is_live_execution_tool` denial (so raw `live_*` stay denied), add:
  ```cpp
  if (mcp::is_fast_live_tool(tool))
      return mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed();
  ```
  (Daemon + headless: as above. GUI `AgentService`: same — it returns true→reach handler when fast-armed, else falls through to the confirmation gate / deny. The handler re-enforces the full stack incl. kill switch + allowed account.) Include `SettingsGate.h` where needed.
- [ ] **Step 3: GUI toggle** — `SecuritySection`: "Arm FAST live mode (advanced)" → `cli.fast_live_armed`, default off, persisted via `repo.set(...,"cli")`, matching the existing arm-toggle pattern.
- [ ] **Step 4: Failing test** `tests/tst_fast_live.cpp` (HeadlessRuntime bring-up, mirror tst_live_trading.cpp; register in tests/CMakeLists.txt): `cli_fast_live_armed()` default false then true; `is_fast_live_tool` true for the 7 names, false for "submit_order"/"live_place_order"/"get_quote"; **keystone:** `set_setting cli.fast_live_armed` (even with settings-write on) → refused + DB unchanged; via `rt_.call_tool`, a fast tool (e.g. `get_positions`) when NOT fast-armed → denied (auth), and the raw `live_place_order` → still denied. RED → implement → GREEN. Full suite green.
- [ ] **Step 5: GUI build + selftest** exit 0. **Commit** `feat(trading): cli.fast_live_armed constitution + fast-live tool gating` + Co-Authored-By.

---

## Task 2: FastLiveTools module + live read tools

**Files:** Create `src/mcp/tools/FastLiveTools.{h,cpp}`; Modify `McpInit.cpp`, `CMakeLists.txt`; Test extend `tests/tst_fast_live.cpp`.

- [ ] **Step 1: Shared gate helper** in FastLiveTools.cpp (anon ns): `struct FastGate { bool ok; QString reason; QString account; };` `FastGate fast_live_gate()`: if `mcp::cli_kill_switch_engaged()` → `{false,"kill switch engaged",""}`; if `!(cli_trading_allowed()&&cli_live_armed()&&cli_fast_live_armed())` → `{false,"fast live mode not armed — arm in GUI Settings",""}`; `acct = cli_allowed_account()`; if empty or `!AccountManager::has_account(acct)` → `{false,"no allowed account configured for AI trading",""}`; else `{true,"",acct}`. (All read LIVE — revocable. Every fast tool calls this first.)
- [ ] **Step 2: read tools** (`get_positions`/`get_open_orders`/`get_fills`, category "fast-live", `is_destructive=false`, no args): each → `auto g = fast_live_gate(); if (!g.ok) return ok_data({status:"rejected",reason:g.reason});` then `broker = AccountManager::broker_for(g.account)` + `creds = load_credentials(g.account)` → `broker->get_positions(creds)` / `get_orders(creds)` / fills (grep the broker iface for the fills accessor; if none, `get_orders` filtered to filled, or `live_get_trades`'s underlying op — state what you used). Shape the result to JSON. Audit optional for reads. Expose `std::vector<ToolDef> get_fast_live_tools()`; register in `McpInit.cpp::register_core_tools`. Add `FastLiveTools.cpp` to CMake (core + GUI exe).
- [ ] **Step 3: Test** — extend tst_fast_live.cpp with the FakeBroker + sandbox account + fast-arm set: `get_positions` returns the fake's positions; NOT fast-armed → rejected "not armed"; kill-switch on → rejected "kill switch engaged"; no allowed account → rejected. RED→GREEN.
- [ ] **Step 4: Commit** `feat(trading): fast-live read tools (get_positions/get_open_orders/get_fills)` + Co-Authored-By.

---

## Task 3: De-risking tools — cancel_order + exit_position

**Files:** Modify `FastLiveTools.cpp`, `McpInit.cpp`; Test extend `tests/tst_fast_live.cpp`.

- [ ] **Step 1: cancel_order** ({order_id}, category "fast-live", is_destructive=true): `fast_live_gate()` → on fail rejected+audit; else `UnifiedTrading::cancel_order(g.account, order_id)` → map response; audit (tool "cancel_order", mode "live", decision cancelled/rejected). NO risk floor (de-risking). A missing order → clean "order not found"/broker message.
- [ ] **Step 2: exit_position** ({symbol, exchange?, product?}, is_destructive=true): `fast_live_gate()`; else `UnifiedTrading::close_position(g.account, symbol, exchange, product)` (reduce-only/flatten); record the close in the live ledger if a fill price is known (best-effort); audit (tool "exit_position", mode "live"). NO order-value floor (de-risking — can only reduce exposure). A missing position → clean "no position".
- [ ] **Step 3: Test** — extend tst_fast_live.cpp (FakeBroker returns success for cancel/close): armed → cancel_order succeeds + audited; armed → exit_position succeeds + audited; NOT armed → both rejected; kill-switch → both rejected. RED→GREEN.
- [ ] **Step 4: Commit** `feat(trading): fast-live de-risking tools (cancel_order, exit_position)` + Co-Authored-By.

---

## Task 4: fast_submit_order (one-shot live)

**Files:** Modify `FastLiveTools.cpp`, `McpInit.cpp`; Test extend `tests/tst_fast_live.cpp`.

- [ ] **Step 1: fast_submit_order** ({symbol, side, quantity, order_type, limit_price?, exchange?}, category "fast-live", is_destructive=true). ONE-SHOT, in order (single call): `fast_live_gate()` → on fail rejected+audit "denied"; build `UnifiedOrder` from the args (reuse the enum parsing from OrderFlowTools or a local parse); resolve the price (limit → limit_price; market → last quote via `peek_quote`, else reject "no price for risk check"); **run the risk floor** (`risk_floor_check(order, price)`); on fail rejected+audit; **daily-loss** (`!daily_loss_ok(rv.max_loss)` → rejected "daily loss limit reached"+audit); then execute `UnifiedTrading::place_order(g.account, order)`; on a fill `record_open`/`record_close` (by side) in the live ledger; audit (tool "fast_submit_order", mode "live", decision filled/rejected, account, broker order id). Return `ok_data{status, order_id, account, mode:"live"}`. NO `order_drafts` row — one-shot.
- [ ] **Step 2: Test** — extend tst_fast_live.cpp (FakeBroker + sandbox account + fast-arm + cli.allowed_account): happy → "filled" + FakeBroker received the order + live_positions row + audit "fast_submit_order"/live/filled; not-armed → rejected; kill-switch → rejected; oversized (over cli.risk.max_order_value) → rejected at the floor (no broker call); daily-loss pre-seeded over cap → rejected; account-not-allowed → rejected. Neuter the floor once → confirm the oversized test flips. RED→GREEN.
- [ ] **Step 3: Build + selftest + grep** — `--selftest-tools` exit 0; `grep -nE "->place_order|adapter" FastLiveTools.cpp` shows ONLY the gated `UnifiedTrading::place_order(g.account,...)` (no raw adapter).
- [ ] **Step 4: Commit** `feat(trading): fast_submit_order (one-shot gated live)` + Co-Authored-By.

---

## Task 5: replace_order

**Files:** Modify `FastLiveTools.cpp`, `McpInit.cpp`; Test extend `tests/tst_fast_live.cpp`.

- [ ] **Step 1: replace_order** ({order_id, symbol, side, quantity, order_type, limit_price?}, is_destructive=true): `fast_live_gate()`; build the new `UnifiedOrder` + resolve price + **run the risk floor on the whole new order** (proposed: validate the full new order, not the delta) + daily-loss; on pass `UnifiedTrading::modify_order(g.account, order_id, mods)` (build the `QJsonObject mods` from the new params); audit (tool "replace_order", mode "live"). On floor/gate fail → rejected (the original order is left untouched — modify only fires after the gate).
- [ ] **Step 2: Test** — extend tst_fast_live.cpp: armed + valid → replace succeeds (FakeBroker modify returns ok) + audited; oversized new params → rejected at floor (no modify call); not-armed/kill-switch → rejected. RED→GREEN. Full suite green.
- [ ] **Step 3: Commit** `feat(trading): replace_order (gated, risk-floored modify)` + Co-Authored-By.

---

## Task 6: e2e + full regression + acceptance matrix

**Files:** Create `tests/e2e_fast_live.sh`.

- [ ] **Step 1: e2e** `tests/e2e_fast_live.sh` (mirror e2e_live_trade.sh: throwaway profile, cleanup trap, watchdog, leak-safe; NO GUI, NO real creds). Bootstrap DB. HARD asserts (deterministic, no broker account):
  - fast tools denied when NOT fast-armed: `sqlite3` set `cli.allow_trading=true,cli.live_trading_armed=true,cli.fast_live_armed=false`; `mcp call get_positions '{}'` → denied (exit 5/auth or rejected "not armed"); `fast_submit_order` → rejected "not armed".
  - set `cli.fast_live_armed=true` but `cli.allowed_account=''` → `fast_submit_order` → rejected "no allowed account" (armed but no account; no broker call).
  - kill-switch: `cli.kill_switch=true` → `get_positions`/`fast_submit_order` → "kill switch engaged".
  - **raw adapter still denied:** `live_place_order` → denied (exit 5) regardless of fast-arm.
  - keystone: `set_setting cli.fast_live_armed true` over daemon → denied.
  Run it; paste output.
- [ ] **Step 2: Full regression (paste):** `ctest --test-dir /tmp/ot-build-test --output-on-failure` (incl. tst_fast_live + all prior green — A/B/C/strategy-loop unchanged); `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`; selftests exit 0; `--headless mcp list` (fast tools present); re-run `e2e_paper_trade.sh` + `e2e_daemon_smoke.sh` rc 0.
- [ ] **Step 3: Acceptance matrix (PASS/FAIL + where):** (1) fast tools unlocked ONLY when fully fast-armed + allowed account + kill-off — unit + e2e; (2) fast_submit one-shot fills + risk-floored + daily-loss-gated — unit; (3) cancel/exit de-risking gated, no floor — unit; (4) replace risk-floored — unit; (5) reads gated — unit; (6) raw adapter / live_* still denied everywhere — unit + e2e; (7) keystone: AI can't set cli.fast_live_armed/caps/account/kill-switch — unit + e2e; (8) revocable (un-arm / kill mid-session → next fast action denied) — unit; (9) no real money (fake/sandbox); (10) no regression.
- [ ] **Step 4: Commit** `test(trading): fast-live e2e + Phase-D acceptance matrix` + Co-Authored-By.

---

## Self-Review
**Spec coverage:** the 7 tools (T2/T3/T4/T5) ✓; `cli.fast_live_armed` constitution + gating across 3 hosts (T1) ✓; gated wrappers over `UnifiedTrading` ops, raw `live_*` denied (T1 denial unchanged + T4 grep) ✓; one-shot fast_submit with the full gate stack + floor + daily-loss (T4) ✓; cancel/exit de-risking no-floor (T3) ✓; replace floored (T5) ✓; reads gated (T2) ✓; sandbox/fake validation (all tasks) ✓; kill-switch/armed/account all GUI-only + revocable + handler-enforced ✓; audit mode "live" ✓.

**Type consistency:** `cli_fast_live_armed`/`is_fast_live_tool` (T1) used in the 3 checkers + `fast_live_gate` (T2); `fast_live_gate` (T2) used by T3/T4/T5; `risk_floor_check`/`daily_loss_ok`/`record_*` reused (T4/T5); broker ops match `UnifiedTrading.h`.

**SAFETY NOTE (real money):** every fast tool MUST call `fast_live_gate()` (kill→armed→fast-armed→allowed-account) first, and fast_submit/replace MUST pass the risk floor + daily-loss, BEFORE any `UnifiedTrading` broker op. NEVER call a raw adapter / the `live_*` tools' path. NEVER route to an AI-supplied account (only `cli.allowed_account`). NEVER make a `cli.*` control AI-writable. Tests use FakeBroker + sandbox only; real money is the human's account+arm. If any task is tempted to skip a gate for "speed", STOP — the gate stack IS the speed-safe contract.
