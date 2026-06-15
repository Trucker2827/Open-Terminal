# AI-Trading Substrate — Phase A (Equity Paper MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A two-phase `prepare_order` → `submit_order` flow on the proven equity paper rail, with a deterministic risk floor, draft + audit tables, a paper-trading toggle, **live hard-off**, and the keystone that the AI cannot arm its own trading.

**Architecture:** New MCP tools `prepare_order` (non-destructive: validate + risk-floor + draft) and `submit_order` (destructive: load draft → re-check → gate by mode → execute on `UnifiedTrading` paper). Trading toggles are GUI-only (the settings-write path refuses them). The daemon/headless auth-checkers get one narrow `submit_order paper` carve-out. Everything audited.

**Tech Stack:** C++20, Qt6, CMake+Ninja, QtTest. Builds in `openterminal_core` (tools/services) so the daemon + headless + GUI all share it.

**Spec:** `openmarketterminal-qt/docs/design/2026-06-14-ai-trading-substrate-design.md`
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` · test build `/tmp/ot-build-test` · **Branch:** create `feat/ai-trading-substrate-phaseA` off `main` (`de0a2ed`) before Task 1.

**Scope:** Phase A only (equity paper). Live execution is wired-but-hard-off (`submit_order mode=live` → rejected "live trading disabled"). Phase B (prediction-market paper) and Phase C (live arming) are follow-ups.

---

## Verified facts
- `UnifiedOrder` (`src/trading/TradingTypes.h`): `symbol, exchange, side(OrderSide), order_type(OrderType), quantity, price, stop_price, …`. `UnifiedOrderResponse{success, order_id, message, mode}`.
- `UnifiedTrading::place_order(const QString& account_id, const UnifiedOrder&)` (`src/trading/UnifiedTrading.h:32`) — paper/live routing, already paper-default-fail-safe. `pt_place_order` (`src/mcp/tools/PaperTradingTools.cpp:117`) shows the paper-account + UnifiedOrder usage to mirror.
- `set_setting` (`src/mcp/tools/SettingsTools.cpp`): `category "settings"`, `is_destructive=true`, handler args `{key,value,category}`. The GUI-only denylist goes in this handler.
- Auth-checkers to carve out: daemon `src/cli/ServeCommand.cpp:52-56` (read-only: Verified→deny, is_destructive→deny, settings-write→deny); headless `src/core/headless/HeadlessRuntime.cpp:95+` (Verified→deny, settings-write→gate, destructive→`cli_trading_allowed`).
- `SettingsGate.{h,cpp}` (`src/mcp/tools/`): `cli_trading_allowed()`, `cli_settings_write_allowed()`, `is_settings_write_tool()`. Add the new helpers here.
- Migrations: last is `v048`; **next = `v049`**. Register via `register_all_migrations()` (`src/storage/sqlite/migrations/RegisterAllMigrations.cpp`).

### Threat-model decisions (resolved before build — apply across tasks)
- **Risk floor (T3):** `PositionManager` (`src/algo_engine/PositionManager.{h,cpp}`) is **stateful, per-deployment** (caps are constructor args; it offers `bool validate_order_value(double qty, double price) const`) — NOT a clean per-order checker for a tool. Phase A implements a small standalone `RiskFloor` helper (Task 3) that reads caps from **GUI-only `cli.risk.*` keys** with **conservative FINITE defaults** (never "no cap"). Mirror `validate_order_value`'s value formula.
- **Keystone denylist = the `cli.` prefix (T1).** `is_gui_only_setting(key)` returns `key.startsWith("cli.")` — the AI cannot write ANY `cli.*` control knob via the tool layer. This covers the three trading toggles AND the `cli.risk.*` caps in ONE rule, so *raising a risk cap is as impossible for the AI as arming trading* (closes the "bypass the floor through the settings door" gap). **Verified safe:** `cli.*` keys are written ONLY by the GUI (`SecuritySection.cpp` via `repo.set` directly, NOT through `set_setting`) and read by `SettingsGate` — blocking the prefix in `set_setting`'s handler breaks no legitimate flow.
- **`set_setting` is the SOLE tool writing arbitrary settings keys** (`SettingsRepository::set(key,value,cat)` at `SettingsTools.cpp:65`). `set_active_llm` writes the LLM-config repo (enum-constrained, cannot touch `cli.*`). So the denylist in `set_setting`'s handler is the complete keystone — no other write tool exists. The T1 keystone test must also assert the DB value is unchanged (not just that the call failed).
- **Destructive permit chain (T4) — carve-out is necessary AND sufficient.** In the daemon process the active `auth_checker_` is ServeCommand's lambda and it is the SOLE gate: `McpProvider::call_tool` (`McpProvider.cpp:248-284`) fully delegates to the checker; there is NO independent `tls_destructive_allowed` check on the internal-tool path (that flag is read only by the *external* MCP branch `McpService.cpp:384` and the *GUI* AgentService checker — neither on the daemon `submit_order` path). So the `submit_order` carve-out in ServeCommand both (a) lets `submit_order paper` through with nothing lower blocking it, and (b) keeps every OTHER destructive tool denied. Do NOT add redundant thread-local plumbing. T5 must concretely fire `live_place_order` over the daemon bridge and assert DENIED.

---

## File Structure
- Modify: `src/mcp/tools/SettingsGate.{h,cpp}` (new gate helpers + the GUI-only-key denylist), `src/mcp/tools/SettingsTools.cpp` (enforce the denylist in `set_setting`), `src/screens/settings/SecuritySection.{h,cpp}` (two new GUI toggles).
- Create: `src/storage/sqlite/migrations/v049_ai_trading.cpp` (order_drafts + trade_audit), `src/storage/repositories/OrderDraftRepository.{h,cpp}`, `src/storage/repositories/TradeAuditRepository.{h,cpp}`.
- Create: `src/mcp/tools/OrderFlowTools.{h,cpp}` (`prepare_order`, `submit_order`, `list_drafts`, `cancel_draft`) + a standalone `RiskFloor` helper (GUI-only `cli.risk.*` caps; see T3).
- Modify: `src/mcp/McpInit.cpp` (register the new tools in `register_core_tools`), `src/cli/ServeCommand.cpp` + `src/core/headless/HeadlessRuntime.cpp` (the `submit_order` carve-out), `CMakeLists.txt` (+`MigrationRunner.h` decl, register v049).
- Tests: `tests/tst_trading_gate_keystone.cpp`, `tests/tst_order_flow.cpp`, `tests/e2e_paper_trade.sh`.

---

## Task 1: KEYSTONE — GUI-only trading toggles + gate helpers (AI can't arm itself)

**Files:** Modify `src/mcp/tools/SettingsGate.{h,cpp}`, `src/mcp/tools/SettingsTools.cpp`, `src/screens/settings/SecuritySection.{h,cpp}`; Test `tests/tst_trading_gate_keystone.cpp`.

- [ ] **Step 1: Add gate helpers + the GUI-only denylist to `SettingsGate.h`:**
```cpp
/// `cli.allow_paper_trading` == "true" — gates submit_order mode=paper. Default false.
bool cli_paper_trading_allowed();
/// `cli.live_trading_armed` == "true" — the Phase-C live-arming flag. Default false.
bool cli_live_armed();
/// True iff `key` is a CLI/agent-control knob that MUST be GUI-only — the
/// settings-WRITE path refuses to change these even when cli.allow_settings_write
/// is on, so a CLI/AI agent can never arm/enable its own trading OR raise its own
/// risk caps. Implemented as the `cli.` prefix (covers the trading toggles AND the
/// cli.risk.* caps in one rule).
bool is_gui_only_setting(const QString& key);
```
`SettingsGate.cpp`: implement the two readers (mirror `cli_trading_allowed`); `is_gui_only_setting` returns `key.startsWith(QLatin1String("cli."))`. (This is the resolved keystone decision — it blocks the whole `cli.*` namespace, so `cli.allow_trading`, `cli.live_trading_armed`, `cli.allow_paper_trading`, `cli.allow_settings_write`, and the `cli.risk.*` caps are all GUI-only.)

- [ ] **Step 2: Enforce the denylist in `set_setting`** — `SettingsTools.cpp`, at the TOP of the `set_setting` handler (before any write):
```cpp
            const QString key = args.value("key").toString();
            if (mcp::is_gui_only_setting(key))
                return ToolResult::fail("Setting '" + key +
                    "' is GUI-only and cannot be changed via the CLI/agent — toggle it in Settings.");
```
(Place before the existing write logic. Confirm the existing handler's variable names.)

- [ ] **Step 3: Failing test** — `tests/tst_trading_gate_keystone.cpp` (links `openterminal_core Qt6::Core Qt6::Test`): with a temp HOME + opened DB,
  - `cli_paper_trading_allowed()`/`cli_live_armed()` default false; true after writing the key directly to the DB.
  - `is_gui_only_setting("cli.allow_trading")` / `"cli.live_trading_armed"` / `"cli.allow_paper_trading"` / `"cli.risk.max_order_value"` → true; `is_gui_only_setting("general.theme")` → false.
  - **The keystone:** call the `set_setting` tool (via `McpProvider::call_tool`) with `{key:"cli.allow_trading", value:"true"}` → result `success==false` and the error mentions GUI-only; **confirm the DB value did NOT change** (`SettingsRepository::get("cli.allow_trading")` still default/absent). Repeat for `cli.risk.max_order_value` (proves the floor can't be raised via the tool). (Even simulate `cli.allow_settings_write=true` first — still refused.)
  Run → FAIL (helpers/denylist absent) → implement → PASS:
```bash
cmake --build /tmp/ot-build-test --target tst_trading_gate_keystone && ctest --test-dir /tmp/ot-build-test -R tst_trading_gate_keystone --output-on-failure
```

- [ ] **Step 4: GUI toggles** — add two toggles to `SecuritySection` ("Allow CLI paper trading" → `cli.allow_paper_trading`; "Arm CLI LIVE trading (advanced)" → `cli.live_trading_armed`), default off, persisted via `SettingsRepository`, matching the existing two toggles' pattern. (These are the ONLY way to change those keys.)

- [ ] **Step 5: Build GUI + selftest:** `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal && "$APP" --selftest-tools; echo exit=$?` → exit 0.

- [ ] **Step 6: Commit** `feat(trading): GUI-only trading toggles + gate helpers (AI cannot arm its own trading)` + Co-Authored-By.

---

## Task 2: Storage — order_drafts + trade_audit (migration v049 + repositories)

**Files:** Create `src/storage/sqlite/migrations/v049_ai_trading.cpp`, `src/storage/repositories/OrderDraftRepository.{h,cpp}`, `src/storage/repositories/TradeAuditRepository.{h,cpp}`; Modify `src/storage/sqlite/migrations/MigrationRunner.h`, `RegisterAllMigrations.cpp`, `CMakeLists.txt`; Test `tests/tst_order_flow.cpp` (storage portion).

- [ ] **Step 1: Migration v049** — `v049_ai_trading.cpp` (mirror an existing `vNNN_*.cpp`): create
  - `order_drafts(draft_id TEXT PK, intent_json TEXT, risk_verdict_json TEXT, account TEXT, mode_hint TEXT, status TEXT, created_at TEXT, expires_at TEXT)`
  - `trade_audit(id INTEGER PK AUTOINCREMENT, ts TEXT, phase TEXT, tool TEXT, account TEXT, mode TEXT, intent_json TEXT, decision TEXT, reason TEXT, risk_snapshot_json TEXT)`
  Declare `register_migration_v049()` in `MigrationRunner.h`; add its call (in order) to `register_all_migrations()`; add the .cpp to `STORAGE_SOURCES` in `CMakeLists.txt`.

- [ ] **Step 2: Repositories** — `OrderDraftRepository` (`insert(draft)`, `get(draft_id)`, `update_status(draft_id, status)`) and `TradeAuditRepository` (`append(row)`), each a thin SQLite wrapper over the shared `Database::instance().connection()` (mirror an existing repository). Append-only for trade_audit (insert only). Add to `STORAGE_SOURCES`.

- [ ] **Step 3: Test (storage)** — in `tests/tst_order_flow.cpp` (links `openterminal_core`): temp HOME, `register_all_migrations()` + `Database::open`; assert `order_drafts` + `trade_audit` tables exist; a draft round-trips (insert→get→update_status); an audit row appends + reads back. Run → FAIL (tables/repos absent) → implement → PASS.

- [ ] **Step 4: Commit** `feat(storage): order_drafts + trade_audit (v049) for the AI-trading substrate` + Co-Authored-By.

---

## Task 3: `prepare_order` — validate + risk floor + draft (non-destructive)

**Files:** Create `src/mcp/tools/OrderFlowTools.{h,cpp}` (+ a `RiskFloor` helper); Modify `src/mcp/McpInit.cpp`, `CMakeLists.txt`; Test extend `tests/tst_order_flow.cpp`.

- [ ] **Step 1: RiskFloor helper (decided — standalone, GUI-only caps).** `PositionManager` is stateful/per-deployment (see Threat-model decisions), so implement a standalone `RiskFloor::check(const UnifiedOrder& o, const QString& account) -> {bool ok; QString reason; double order_value; double max_loss;}` in OrderFlowTools.cpp. Read caps from the **GUI-only `cli.risk.*`** keys via `SettingsRepository::get`, each with a **conservative FINITE default** (NOT "no cap"):
  - `cli.risk.max_order_value` default `25000.0`
  - `cli.risk.max_position_qty` default `10000.0`
  - `cli.risk.max_daily_loss` default `5000.0`
  Compute `order_value = quantity * price` (for market orders use the last quote from DataHub; if no price is resolvable, reject "no price for risk check"). Reject if `order_value > max_order_value` ("exceeds max order value") or `quantity > max_position_qty` ("exceeds max position size"). Mirror `PositionManager::validate_order_value`'s value formula. (Caps are GUI-only by the T1 prefix rule, so the AI cannot raise them — the floor is unbypassable.)

- [ ] **Step 2: `prepare_order` tool** (category `trading`, **is_destructive=false**, auth None): parse the structured intent (`symbol, side, quantity, order_type, limit_price?, account?, strategy?, reason?`) → build `UnifiedOrder`; validate (symbol non-empty, side/order_type valid, account resolves — default to the paper account; limit order needs a price); run the risk floor; on pass persist an `order_drafts` row (status `prepared`, `expires_at` = now+5min) + append a `trade_audit` (phase `prepare`, decision `prepared`) and return `{status:"prepared", draft_id, risk_status:"passed", max_loss, checks:[...]}`. On fail: audit (decision `rejected`) + return `{status:"rejected", reason, checks}`. Register in `register_core_tools` (McpInit.cpp). Add OrderFlowTools.cpp to `MCP_SOURCES`/core.

- [ ] **Step 3: Test** — extend `tst_order_flow.cpp`: a valid intent → `prepare_order` returns `prepared` + a draft exists + an audit row; an oversized intent (over the default `cli.risk.max_order_value`=25000, e.g. qty 1000 @ 200) → `rejected` with a risk reason + an audit `rejected` row + NO usable draft. Run → FAIL → implement → PASS.

- [ ] **Step 4: Commit** `feat(trading): prepare_order — validate + risk floor + draft` + Co-Authored-By.

---

## Task 4: `submit_order` + the daemon/headless carve-out (paper executes; live hard-off)

**Files:** Modify `src/mcp/tools/OrderFlowTools.cpp`, `src/mcp/McpInit.cpp`, `src/cli/ServeCommand.cpp`, `src/core/headless/HeadlessRuntime.cpp`; Test extend `tests/tst_order_flow.cpp`.

- [ ] **Step 1: `submit_order` tool** (category `trading`, **is_destructive=true**, auth Authenticated): args `{draft_id, mode}`. Load the draft (must exist, status `prepared`, not past `expires_at` — else `rejected "draft expired/invalid"`). **RE-RUN** the risk floor + validation against fresh state. Gate by mode:
  - `mode=="paper"`: require `mcp::cli_paper_trading_allowed()` else `{status:"rejected","reason":"paper trading disabled — enable in GUI Settings"}`. Then execute `UnifiedTrading::place_order(paper_account, order)`; map `UnifiedOrderResponse` → result; update draft (`submitted`); audit (phase `submit`, decision `filled`/`rejected`, risk snapshot).
  - `mode=="live"`: **Phase A hard-off** → `{status:"rejected","reason":"live trading disabled (paper-first; not yet enabled)"}` + audit (decision `denied`). (Do NOT wire live execution — Phase C.)
  - else → `rejected "unknown mode"`.
  Register in `register_core_tools`.

- [ ] **Step 2: The carve-out — daemon** (`ServeCommand.cpp:52-56` read-only checker). Add a `submit_order` branch BEFORE the `is_destructive → deny`:
```cpp
        [](const QString& tool, const QJsonObject& args, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (tool == "submit_order") {
                const QString mode = args.value("mode").toString();
                if (mode == "paper") return mcp::cli_paper_trading_allowed();
                // live requires arming (Phase C); denied in Phase A even if armed
                return mcp::cli_trading_allowed() && mcp::cli_live_armed();  // false in Phase A (toggles off)
            }
            if (is_destructive) return false;
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });
```
(The live branch is structurally present but resolves false in Phase A since the toggles default off + submit_order's handler hard-offs live anyway — defense in depth.) Include `SettingsGate.h`. **This lambda is the SOLE daemon gate** (no lower thread-local check — see Threat-model decisions); so it must return true ONLY for `submit_order paper` (toggle on) and false for every other destructive tool — which the structure above does.

- [ ] **Step 3: The carve-out — headless** (`HeadlessRuntime.cpp` checker). Add the same `submit_order` branch (paper→`cli_paper_trading_allowed`; live→`cli_trading_allowed && cli_live_armed`) before its destructive handling, so `openterminalcli --headless submit_order` honors the paper gate too.

- [ ] **Step 4: Test** — extend `tst_order_flow.cpp`: prepare a valid intent, then `submit_order paper` with `cli.allow_paper_trading` written true → `filled` (or a clean paper result) + draft `submitted` + audit `submit` row; with the toggle false → `rejected "paper trading disabled"`; `mode=live` → `rejected "live trading disabled"` regardless; an expired/missing draft → `rejected`; a draft whose risk now fails the re-check (write `cli.risk.max_order_value` to a value below the prepared order's value between prepare and submit) → `rejected` at submit (proves re-validation reads fresh caps). Run → FAIL → implement → PASS.

- [ ] **Step 5: Build + headless smoke (no GUI):**
```bash
cmake --build /tmp/ot-build-ht --target openterminalcli
# paper toggle is GUI-only; for the smoke, set it via sqlite directly OR a temp test profile where the test wrote it.
/tmp/ot-build-ht/openterminalcli --headless mcp call prepare_order '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"market"}'; echo "exit=$?"
/tmp/ot-build-ht/openterminalcli --headless mcp call submit_order '{"draft_id":"<from prepare>","mode":"live"}'; echo "exit=$?"   # live disabled -> exit 5
```
Expected: prepare returns a draft; submit live → rejected "live trading disabled" (exit 5).

- [ ] **Step 6: Commit** `feat(trading): submit_order (paper executes, live hard-off) + daemon/headless carve-out` + Co-Authored-By.

---

## Task 5: End-to-end paper cycle + full regression (the spec's test matrix)

**Files:** Create `tests/e2e_paper_trade.sh`.

- [ ] **Step 1: e2e script** — `tests/e2e_paper_trade.sh` (chmod +x), throwaway `--profile paper-e2e-$$`, NO GUI:
  - Arm paper by writing `cli.allow_paper_trading=true` into the throwaway profile's DB via the bridge/headless is impossible (GUI-only) — so the script writes it directly with `sqlite3` on the throwaway profile DB (documented: simulating the human GUI toggle), OR runs the daemon and notes the toggle must be set in Settings. Then:
  - start `serve` on the profile; `prepare_order` (valid) → capture `draft_id`; `submit_order {draft_id, paper}` → assert `filled`/success; `submit_order {…, live}` → assert rejected "live trading disabled"; `set_setting cli.allow_trading true` → assert REFUSED (keystone); **`live_place_order {…}` over the daemon bridge → assert DENIED** (proves the carve-out is `submit_order`-only and did not open other destructive tools); `serve --stop`.
  - Use `timeout` watchdogs; assert no hang; clean up the profile.
- [ ] **Step 2: Run it; paste output** (each assertion PASS/FAIL).
- [ ] **Step 3: Full regression (paste):** `ctest --test-dir /tmp/ot-build-test --output-on-failure` (all tst_* incl the new three); `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`; GUI selftests loop (all exit 0); headless one-shot `--headless mcp list | head -c 80`.
- [ ] **Step 4: Map to the spec's test matrix (report each PASS/FAIL):** happy paper path; risk-floor reject (prepare + submit re-check); gate ladder (paper off→denied, live→disabled); **keystone (set_setting refuses trading keys even with settings-write on)**; revocable (toggle paper off mid-session → next submit denied); audit completeness (rows for prepare/submit/reject); no-regression (selftests + attach + headless).
- [ ] **Step 5: Commit** `test(trading): paper-cycle e2e + Phase-A acceptance matrix` + Co-Authored-By.

---

## Self-Review
**Spec coverage:** two-phase (T3 prepare, T4 submit) ✓; risk floor at prepare+submit (T3 + T4 re-check) ✓; draft+audit tables (T2) ✓; gate ladder + carve-out (T4) ✓; keystone GUI-only toggles (T1) ✓; revocable live-recheck (T4 submit re-reads settings; no token) ✓; live hard-off (T4) ✓; audit (T2/T3/T4) ✓; e2e + matrix (T5) ✓. Phase B/C deferred (noted).

**Resolved before build (was discovery, now decided):** risk-floor source = standalone `RiskFloor` over GUI-only `cli.risk.*` caps with finite defaults (T3); keystone = `cli.` prefix denylist in the sole writer `set_setting` (T1); daemon carve-out is the sole gate, no lower thread-local check (T4). **Remaining minor groundings (with fallbacks):** paper-account id resolution (mirror `pt_place_order` at `PaperTradingTools.cpp:117`); exact `set_setting` handler var names (`SettingsTools.cpp:57-65`).

**Type consistency:** `cli_paper_trading_allowed`/`cli_live_armed`/`is_gui_only_setting` (T1) used in T4 carve-outs; `order_drafts`/`trade_audit`/the repos (T2) used in T3/T4; `prepare_order`/`submit_order` names consistent; `UnifiedOrder`/`UnifiedTrading::place_order` match the real API.

**Placeholder scan:** code steps carry real code; the risk-floor + paper-account + handler-var spots are grep-grounded with fallbacks, not vague.

**SAFETY NOTE for executors:** Phase A must ship with live execution NOT wired (submit_order live = a string rejection, no broker call) and the three trading toggles GUI-only. If any task is tempted to wire live or make a toggle CLI-writable, STOP — that violates the spec's keystone.
