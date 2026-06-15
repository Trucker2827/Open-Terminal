# AI-Trading Substrate — Phase C (Human-Enabled AI Live Mode) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax. **This is the REAL-MONEY phase — every safety gate is load-bearing. Build + test ONLY against sandbox accounts; never hardcode or test with real credentials.**

**Goal:** Turn the `submit_order mode:"live"` hard-off into gated live execution on BOTH rails (equity broker + PM adapter), behind the full human-owned constitution: kill switch, two-key arm, an allowed account, the deterministic risk floor, and an enforced daily-loss limit. `submit_order` live only (no cancel/replace). Validated against a broker SANDBOX account — real money is solely a human credential/mode choice the AI cannot make.

**Architecture:** The daemon `submit_order` carve-out ALREADY allows live through to the handler iff `cli_trading_allowed() && cli_live_armed()` (built in Phase A) — so NO checker change. Phase C: (1) add `cli.kill_switch` + `cli.allowed_account` constitution; (2) add a live realized-P&L ledger + enforce `cli.risk.max_daily_loss`; (3) replace the two live hard-offs (equity + PM) with gated execution + record fills; the equity rail is `UnifiedTrading::place_order(account_id, order)` (sync), the PM rail is `adapter->place_order(OrderRequest)` (async, bridged). The account's human-set mode (`paper`/`sandbox`/`live`) decides sandbox-vs-real-money; the AI never sees it.

**Tech Stack:** C++20, Qt6 (Core/Network/Sql/Concurrent), CMake+Ninja, QtTest.

**Spec:** `docs/design/2026-06-15-ai-trading-substrate-phaseC-design.md` · **Parent:** `2026-06-14-ai-trading-substrate-design.md`.
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` (bundle `.../OpenTerminal.app/Contents/MacOS/OpenTerminal`, CLI `/tmp/ot-build-ht/openterminalcli`) · test build `/tmp/ot-build-test` · **Branch:** create `feat/ai-trading-substrate-phaseC` off `main` before Task 1.

---

## Verified facts (grounded — trust these)
- **Two live hard-off sites in `src/mcp/tools/OrderFlowTools.cpp`** to replace with gated execution: equity submit (~line 889, in the `submit_order` handler's equity branch) and PM submit (~line 592, in `submit_prediction_order`). Both currently `audit_submit("denied", "live trading disabled ...")` + return rejected. Phase C replaces each with the gated live path below; the PAPER paths stay unchanged.
- **Carve-out is already live-correct** (Phase A): `ServeCommand.cpp` + `HeadlessRuntime.cpp` `submit_order` branch returns `cli_trading_allowed() && cli_live_armed()` for non-paper mode → the handler is reached only when armed. **No checker change.**
- **Equity live rail (SYNC):** `trading::UnifiedTrading::instance().place_order(const QString& account_id, const UnifiedOrder& order)` → `UnifiedOrderResponse{ bool success; QString order_id; QString message; QString mode; }`. Routes by the account's mode (`live`/`sandbox`/`demo` → broker; else paper sim) — existing fail-safe. The order is the same `UnifiedOrder` the equity prepare path builds (`build_order_from_intent`).
- **Account model:** `trading::AccountManager::instance()` — `BrokerAccount get_account(const QString& account_id) const` (READ `BrokerAccount` in `AccountManager.h`/`TradingTypes.h` for the exact **mode** field name — likely `mode`/`trading_mode`), `bool has_account(id)`, `load_credentials(id)`, `set_trading_mode(id, mode)`. The AI never reads credentials; the handler passes only the account id to `place_order`.
- **PM live rail (ASYNC):** `PredictionExchangeRegistry::instance().adapter(venue)->place_order(const OrderRequest& req)` emits `order_placed(const OrderResult&)` or `error_occurred(ctx,msg)`. READ `OrderRequest` (`PredictionTypes.h:143` — `{key, asset_id, side "BUY"/"SELL", order_type, price, size, expires_ms, client_order_id, extras}`) + `OrderResult` (`:155`). Build the `OrderRequest` from the PM intent (side upper-case, price=resolved fill price, size=contracts, `client_order_id`=a uuid you set). Bridge to sync via `detail::run_async_wait` using the SAME correlated+timed pattern as Task 2's `pm_fetch_*` — correlate the `order_placed` result by `client_order_id` if `OrderResult` carries it, else accept the first `order_placed` (single-worker host) + the 15s timeout; bind `error_occurred` terminal.
- **SettingsGate** (`src/mcp/tools/SettingsGate.{h,cpp}`, ns `openmarketterminal::mcp`): `cli_trading_allowed()`, `cli_live_armed()`, `cli_paper_trading_allowed()`, `cli_venue_allowed()`, `is_gui_only_setting()` (the `cli.` prefix — so ALL new `cli.*` keys are GUI-only for free; no keystone change). `flag_true(key)` helper. `read_cap` is in OrderFlowTools.cpp.
- **Migrations:** last is `v050`; **next = `v051`**. Pattern = `v050_pm_paper.cpp`.
- **Repos:** `BaseRepository<T>`; `db().execute(sql,params)` → `Result<QSqlQuery>` (`.value().numRowsAffected()` / iterate). Mirror `PmPaperRepository`.
- **Audit:** `audit_submit(account, mode, intent, decision, reason, risk_verdict)` in OrderFlowTools.cpp; `trade_audit.reason` is NOT NULL → always pass a non-empty reason.
- **Engine reuse:** `PmPaperEngine` cost-basis math (`PmPaperEngine.cpp`) is the model for the live realized-P&L ledger.

---

## File Structure
- Create: `src/storage/sqlite/migrations/v051_live_pnl.cpp`; `src/storage/repositories/LivePnlRepository.{h,cpp}` (live positions ledger + daily_pnl); `src/mcp/tools/LivePnl.{h,cpp}` (realize-on-fill + today's-realized-loss helpers) — OR fold the helpers into OrderFlowTools.cpp if smaller.
- Modify: `src/mcp/tools/SettingsGate.{h,cpp}` (`cli_kill_switch_engaged`, `cli_allowed_account`); `src/mcp/tools/OrderFlowTools.cpp` (kill-switch gate in prepare+submit; replace BOTH live hard-offs with gated execution; daily-loss gate; record fills); `src/screens/settings/SecuritySection.{h,cpp}` (kill-switch toggle, allowed-account field, max-daily-loss field); `src/storage/sqlite/migrations/MigrationRunner.h` + `RegisterAllMigrations.cpp`; `CMakeLists.txt`; `tests/CMakeLists.txt`.
- Tests: `tests/tst_live_trading.cpp` (gates + ledger + both rails via fakes); `tests/e2e_live_trade.sh` (deterministic gates over the daemon + best-effort Alpaca sandbox).

---

## Task 1: Kill switch + allowed-account constitution

**Files:** Modify `src/mcp/tools/SettingsGate.{h,cpp}`, `src/mcp/tools/OrderFlowTools.cpp`, `src/screens/settings/SecuritySection.{h,cpp}`; Test `tests/tst_live_trading.cpp`.

- [ ] **Step 1: Gate helpers** — `SettingsGate.h`: `bool cli_kill_switch_engaged();` (true iff `cli.kill_switch`=="true", default false) and `QString cli_allowed_account();` (reads `cli.allowed_account`, default ""). `SettingsGate.cpp`: implement (mirror `flag_true` / the venue reader). (No keystone change — `cli.kill_switch` + `cli.allowed_account` are GUI-only via the prefix.)
- [ ] **Step 2: Enforce the kill switch** — in OrderFlowTools.cpp, at the TOP of BOTH the `prepare_order` and `submit_order` handlers (before any work, both asset classes): `if (mcp::cli_kill_switch_engaged()) { audit (...,"kill_switch","kill switch engaged"); return ToolResult::ok_data({{"status","rejected"},{"reason","kill switch engaged"}}); }` (use the existing audit helper; for prepare use `audit_prepare`). The kill switch halts paper AND live.
- [ ] **Step 3: Failing test** `tests/tst_live_trading.cpp` (HeadlessRuntime+QTemporaryDir bring-up, mirror tst_pm_paper.cpp; register in tests/CMakeLists.txt; link `openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test`): `cli_kill_switch_engaged()` default false, true after `SettingsRepository::set("cli.kill_switch","true","cli")`; `cli_allowed_account()` "" then "acct-1" after set; with the kill switch on, a `prepare_order` (any valid equity intent) and a `submit_order` both return status "rejected" reason "kill switch engaged"; **keystone:** `set_setting` on `cli.kill_switch`/`cli.allowed_account` (even with `cli.allow_settings_write` on) → refused + DB unchanged. RED → implement → GREEN.
- [ ] **Step 4: GUI fields** — `SecuritySection`: a "Kill switch (halt all AI trading)" toggle → `cli.kill_switch`; an "AI-allowed account id" field → `cli.allowed_account`; a "Max daily loss ($)" field → `cli.risk.max_daily_loss`. Persist via `repo.set(...,"cli")`, match the existing toggle/field pattern.
- [ ] **Step 5: Build GUI + selftest** `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal && .../OpenTerminal --selftest-tools; echo exit=$?` → 0.
- [ ] **Step 6: Commit** `feat(trading): kill switch + allowed-account constitution (Phase C)` + Co-Authored-By.

---

## Task 2: Live realized-P&L ledger + daily-loss enforcement

**Files:** Create `src/storage/sqlite/migrations/v051_live_pnl.cpp`, `src/storage/repositories/LivePnlRepository.{h,cpp}`, `src/mcp/tools/LivePnl.{h,cpp}`; Modify `MigrationRunner.h`, `RegisterAllMigrations.cpp`, `CMakeLists.txt`; Test extend `tests/tst_live_trading.cpp`.

> **Scope note (honest boundary):** accurate live realized P&L would need per-order broker fill reconciliation (a large subsystem). For this MVP the ledger tracks realized P&L from the substrate's OWN round-trips at KNOWN prices (the resolved/submitted fill price + the substrate's tracked cost basis) — EXACT for sandbox/paper fills, an approximation-by-submitted-price for real equity fills. This is deterministic, in-daemon, conservative (halts on breach), and AI-uncontrolled. True broker-fill reconciliation is a documented follow-up.

- [ ] **Step 1: Migration v051** `v051_live_pnl.cpp` (mirror v050):
  - `live_positions(id INTEGER PK AUTOINCREMENT, account TEXT NOT NULL, venue TEXT NOT NULL DEFAULT '', instrument TEXT NOT NULL, qty REAL NOT NULL DEFAULT 0, avg_cost REAL NOT NULL DEFAULT 0, cost_basis REAL NOT NULL DEFAULT 0, opened_at TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'open')` (no UNIQUE; one-open invariant via a status='open' query). `instrument` = equity symbol or PM asset_id; `venue` distinguishes the rail.
  - `daily_pnl(utc_day TEXT PRIMARY KEY, realized_pnl REAL NOT NULL DEFAULT 0, updated_at TEXT NOT NULL DEFAULT '')`.
  Wire MigrationRunner.h + RegisterAllMigrations.cpp + CMakeLists STORAGE list.
- [ ] **Step 2: LivePnlRepository** (mirror PmPaperRepository): `get_open(account, venue, instrument)` (status='open'), `insert_open(...)`, `set_position(id, qty, avg_cost, cost_basis, status)`, `Result<double> realized_today()` (SELECT realized_pnl FROM daily_pnl WHERE utc_day=? — the caller passes today's UTC day; 0 if absent), `Result<void> add_realized(const QString& utc_day, double delta)` (upsert: realized_pnl += delta). Add to STORAGE list.
- [ ] **Step 3: LivePnl helpers** `src/mcp/tools/LivePnl.{h,cpp}` (ns `openmarketterminal::mcp::tools`):
  - `void record_open(account, venue, instrument, qty, fill_price)` — upsert the open position (weighted-avg cost), mirroring PmPaperEngine buy_to_open's avg/cost math (NO cash — this ledger is for P&L only).
  - `double record_close(account, venue, instrument, qty, fill_price)` — reduce the open position pro-rata; realized = `(fill_price - avg_cost) * closed_qty` (sign per side handled by the caller passing a close); `add_realized(today_utc, realized)`; return realized. If no open position, realized 0 (a close of an untracked position contributes nothing — conservative).
  - `bool daily_loss_ok(double prospective_max_loss)` — `realized_today()` is a signed P&L; today's LOSS = `max(0, -realized_today)`; return `(today_loss + prospective_max_loss) <= read_cap("cli.risk.max_daily_loss", 5000)`. (Caller passes the order's max-loss; a breach → false.) Helper `today_utc()` = `QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate)`.
- [ ] **Step 4: Test** — extend tst_live_trading.cpp: ledger open→close realizes the right P&L into daily_pnl; `daily_loss_ok` true under the cap, false once realized loss + prospective breaches `cli.risk.max_daily_loss`; a profit (positive realized) does not reduce headroom below 0 (today_loss floored at 0). RED → implement → GREEN; full suite green.
- [ ] **Step 5: Commit** `feat(trading): live realized-P&L ledger + daily-loss enforcement (v051)` + Co-Authored-By.

---

## Task 3: `submit_order` LIVE — equity rail (gated execution)

**Files:** Modify `src/mcp/tools/OrderFlowTools.cpp`; Test extend `tests/tst_live_trading.cpp`.

- [ ] **Step 1: Replace the equity live hard-off (~line 889)** with the gated live path. The handler already loaded the draft + re-resolved + re-ran the equity risk floor fresh (Phase A) and reserved. For `mode=="live"` (equity), in order (all live-read — revocable; kill switch already checked at the top from Task 1):
  1. `if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed())) → ok_data{rejected,"live trading not armed — arm in GUI Settings"}` + audit "denied". (Defense in depth behind the checker.)
  2. `const QString acct = mcp::cli_allowed_account(); if (acct.isEmpty() || !trading::AccountManager::instance().has_account(acct)) → rejected "no allowed account configured for AI trading"` + audit. (If the intent/draft names an account, it MUST equal `acct`, else reject "account not allowed".)
  3. Daily-loss: `if (!daily_loss_ok(rv.max_loss)) → rejected "daily loss limit reached"` + audit "denied".
  4. `reserve_for_submit` (already in the Phase A path — keep it) → execute: `auto resp = trading::UnifiedTrading::instance().place_order(acct, order);` Map: `update_status(draft_id, resp.success?"submitted":"submit_failed")`; on success record the fill in the live ledger (`record_open`/`record_close` by side — a BUY opens/adds, a SELL closes; use the resolved fill price); `audit_submit(acct,"live",intent, resp.success?"filled":"rejected", resp.message.isEmpty()? (resp.success?"live order placed":"broker rejected") : resp.message, rv)`; return `ok_data{status: resp.success?"filled":"rejected", order_id: resp.order_id, account: acct, mode:"live", message: resp.message}`.
- [ ] **Step 2: FakeBroker / sandbox seam for tests** — the test cannot hit a real broker. Two options (pick the cleaner against the real `UnifiedTrading`/`AccountManager` API — investigate): (a) configure a test account in `sandbox` mode whose broker is a fake/stub that returns a deterministic `UnifiedOrderResponse{success:true,...}`; or (b) if `UnifiedTrading` has no injection seam, add a minimal test-only broker registered under a fake broker id and an account in `sandbox` mode pointing at it. State which you used. The point: the test arms live (`cli.allow_trading`+`cli.live_trading_armed`=true), sets `cli.allowed_account` to the fake account, and asserts an equity `submit_order live` FILLS via the fake + records the live ledger + audits mode "live".
- [ ] **Step 3: Test (deterministic gates — the safety core, no broker needed)** — extend tst_live_trading.cpp:
  - **not armed:** `cli.live_trading_armed=false` (or allow_trading off) → equity `submit_order live` → rejected "not armed", no execution.
  - **no allowed account:** armed but `cli.allowed_account` empty → rejected "no allowed account".
  - **account mismatch:** intent names a different account than `cli.allowed_account` → rejected "account not allowed".
  - **daily-loss halt:** pre-seed `daily_pnl` with a realized loss ≥ `cli.risk.max_daily_loss` → armed live submit → rejected "daily loss limit reached".
  - **kill switch dominates:** kill switch on + fully armed → rejected "kill switch engaged".
  - **happy (fake broker):** fully armed + allowed fake-sandbox account + within caps → "filled", live ledger updated, audit mode "live".
  - **revocable:** un-arm between prepare and submit → rejected.
  RED → implement → GREEN; equity Phase A paper path UNCHANGED (tst_order_flow passes).
- [ ] **Step 4: Build + selftest + grep** GUI `--selftest-tools` exit 0; confirm the equity live path calls `UnifiedTrading::place_order(acct, order)` ONLY when fully gated (read the branch).
- [ ] **Step 5: Commit** `feat(trading): submit_order live equity (gated broker execution)` + Co-Authored-By.

---

## Task 4: `submit_order` LIVE — prediction-market rail (gated execution)

**Files:** Modify `src/mcp/tools/OrderFlowTools.cpp` (+ a PM live-place bridge helper, reuse Task-2 bridge style); Test extend `tests/tst_live_trading.cpp`.

- [ ] **Step 1: PM live-place bridge** — a helper `pm_place_live(venue, OrderRequest) -> {bool ok; QString reason; QString order_id;}` using `detail::run_async_wait` over `registry.adapter(venue)`: connect `order_placed(OrderResult)` (correlate by `client_order_id` if `OrderResult` carries it, else accept first on the single-worker host) → capture result + `signal_done`; connect `error_occurred` → terminal; mandatory 15s `QTimer::singleShot` timeout; THEN call `adapter->place_order(req)`. (Same lifetime-safe heap-state pattern as `pm_fetch_order_book`.) READ `OrderResult` for the success/order-id field names.
- [ ] **Step 2: Replace the PM live hard-off (~line 592 in `submit_prediction_order`)** with the gated live path (same gate order as equity): armed? → allowed-account (PM may key the account differently — if PM uses `cli.allowed_venues` + credentials rather than `cli.allowed_account`, gate on `cli_venue_allowed(venue)` AND require the adapter `has_credentials()`; state the choice) → daily-loss(`rv.max_loss`) → reserve → build `OrderRequest` from the PM intent (side upper, price=resolved fill, size=contracts, client_order_id=uuid) → `pm_place_live(venue, req)` → on ok record the fill in the live ledger (buy→record_open, sell→record_close, instrument=asset_id, venue=venue) + update draft + audit mode "live" → return. Live never calls the adapter `place_order` unless fully gated.
- [ ] **Step 3: Test** — extend tst_live_trading.cpp with a FakePredictionAdapter (extend the Task-2 fake) whose `place_order` emits `order_placed` with a fixture OrderResult: armed + venue allowed + creds + within caps → PM `submit_order live` "filled" + live ledger + audit mode "live"; not armed → rejected; venue not allowed → rejected; daily-loss halt → rejected; kill switch → rejected. RED → implement → GREEN; PM Phase B paper path UNCHANGED (tst_pm_paper passes).
- [ ] **Step 4: Build + selftest + grep** GUI `--selftest-tools` exit 0; `grep -nE "adapter.*place_order|->place_order" src/mcp/tools/OrderFlowTools.cpp` → the only adapter `place_order` is inside the fully-gated PM live branch (read it).
- [ ] **Step 5: Commit** `feat(trading): submit_order live prediction-market (gated adapter execution)` + Co-Authored-By.

---

## Task 5: e2e (deterministic gates + sandbox best-effort) + full regression + acceptance matrix

**Files:** Create `tests/e2e_live_trade.sh`.

- [ ] **Step 1: e2e** `tests/e2e_live_trade.sh` (mirror `tests/e2e_paper_trade.sh`: cleanup trap, fixed profile names, watchdogs, leak-safe). NO GUI, NO real credentials. The daemon has no armed live account by default, so HARD-assert the deterministic constitution:
  - bootstrap profile DB; `sqlite3` set `cli.allow_trading=true`, `cli.live_trading_armed=true`, `cli.allowed_account=''` (simulate a human who armed but configured no account), `cli.kill_switch=false`. Start `serve`.
  - HARD asserts (no network): `submit_order {draft_id:"nope", mode:"live"}` → rejected (no allowed account / draft-not-found — NOT a fill); set `cli.kill_switch=true` via sqlite → `prepare_order` (valid) over daemon → rejected "kill switch engaged"; `set_setting {key:"cli.kill_switch",value:"false"}` over the daemon → denied (exit 5 + "auth", keystone); `set_setting {key:"cli.allowed_account",value:"x"}` → denied (keystone); `live_place_order {}` over daemon → still denied (exit 5; carve-out unchanged).
  - Best-effort (only if `ALPACA_SANDBOX_KEY`/`SECRET` env present — otherwise SKIP with a logged note): configure a sandbox account, arm, `prepare_order`+`submit_order live` a tiny order → assert a fill + a mode-"live" audit row. Document that real-money is never exercised.
  - `serve --stop`; leak-safe cleanup. `chmod +x`.
- [ ] **Step 2: Run it; paste output.**
- [ ] **Step 3: Full regression (paste):** `ctest --test-dir /tmp/ot-build-test --output-on-failure` (all tst_*, incl new `tst_live_trading` + UNCHANGED `tst_order_flow`/`tst_pm_paper`); `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`; every `--selftest-*` exit 0; headless `mcp list`; re-run `bash tests/e2e_paper_trade.sh` + `bash tests/e2e_pm_paper_trade.sh` + `bash tests/e2e_daemon_smoke.sh` (no A/B regression) → rc 0.
- [ ] **Step 4: Acceptance matrix (each PASS/FAIL + where proven):** (1) live executes ONLY when fully armed + allowed account + within caps + kill-off — unit (fake broker/adapter, both rails) + sandbox best-effort; (2) kill switch halts paper+live, checked first — unit + e2e; (3) not-armed / no-account / account-mismatch → denied — unit + e2e; (4) daily-loss halt at the cap (realized tally) — unit; (5) keystone: set_setting refuses cli.kill_switch/cli.allowed_account/cli.allow_trading/cli.live_trading_armed/cli.risk.max_daily_loss even with settings-write on — unit + e2e; (6) revocable (un-arm / kill / clear-account mid-session → next submit denied) — unit; (7) audit completeness (mode "live" rows) — unit; (8) no real money: all tests sandbox/fake; (9) no-regression: A equity paper + B PM paper unchanged, daemon smoke green.
- [ ] **Step 5: Commit** `test(trading): live-mode e2e (deterministic gates + sandbox) + Phase-C matrix` + Co-Authored-By.

---

## Self-Review
**Spec coverage:** kill switch (T1, checked first, paper+live) ✓; allowed-account constitution (T1) ✓; both new keys GUI-only (free via `cli.` prefix; readers T1) ✓; daily-loss enforced as realized tally (T2, scoped boundary documented) ✓; equity live gated execution (T3) ✓; PM live gated execution (T4) ✓; arm = two keys re-read live/revocable (T3/T4) ✓; account = the safety lever, mode human-set (T3 uses `place_order(account_id)` which routes by mode) ✓; sandbox-only validation (T3/T4 fakes + T5 sandbox-if-creds) ✓; submit-only (no cancel/replace added) ✓; A/B unchanged (T3/T4 leave paper paths; T5 regression) ✓; live hard-off removed only behind the full gate stack ✓.

**Type consistency:** `cli_kill_switch_engaged`/`cli_allowed_account` (T1) used in T3/T4; `LivePnlRepository`/`record_open`/`record_close`/`daily_loss_ok` (T2) used in T3/T4; the equity rail `UnifiedTrading::place_order(account_id,order)` (sync) and the PM rail `pm_place_live` (async-bridged) match the real APIs; gate order identical across both rails (kill→armed→account→floor→daily-loss→reserve→execute).

**Placeholder scan:** code-shaped steps carry real signatures/SQL/gate logic; the few "confirm the field name / pick the seam" notes (BrokerAccount mode field; OrderResult fields; the fake-broker injection seam; whether PM gates on account vs venue+creds) are grep-grounded with a stated decision point, not vague.

**SAFETY NOTE for executors (real money):** live execution must fire ONLY inside the full gate stack (kill-off AND armed AND allowed-account AND fresh-floor-pass AND daily-loss-ok AND reserved). NEVER call `UnifiedTrading::place_order(account_id,...)` or `adapter->place_order(...)` outside that stack. NEVER weaken a `cli.*` key to CLI-writable (GUI-only via the prefix). NEVER hardcode/commit real credentials; tests use sandbox/fake only. Keep the A/B paper paths behaviorally unchanged. If any task is tempted to broaden the carve-out or skip a gate, STOP.
