# AI-Trading Substrate — Phase B (Prediction-Market Paper) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extend the shipped Phase A substrate to **prediction-market paper** on **Polymarket AND Kalshi** — PM read tools, a PM paper engine, the unified two-phase `prepare_order`/`submit_order` discriminated by `asset_class:"prediction"`, a deterministic PM risk floor on GUI-only caps, BUY-to-open + SELL-to-close only, best-available immediate fill, mark-to-market only, **live PM hard-off**.

**Architecture:** Reuse the Phase A machinery (drafts, audit, gate ladder, the `cli.` constitution, the `submit_order` carve-out, `reserve_for_submit`). Add: a PM paper engine + storage (`pm_paper_positions`/`pm_paper_account`), PM read MCP tools that bridge the async `PredictionExchangeAdapter` to sync via `run_async_wait`, headless/daemon registration of the PM adapters, and PM-class branches in `prepare_order`/`submit_order` + a PM risk floor.

**Tech Stack:** C++20, Qt6 (Core/Network/Sql/Concurrent — NO Widgets in core), CMake+Ninja, QtTest.

**Spec:** `docs/design/2026-06-15-ai-trading-substrate-phaseB-design.md` · **Parent:** `2026-06-14-ai-trading-substrate-design.md`.
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` (bundle `.../OpenTerminal.app/Contents/MacOS/OpenTerminal`, CLI `/tmp/ot-build-ht/openterminalcli`) · test build `/tmp/ot-build-test` (`-DOPENMARKETTERMINAL_BUILD_TESTS=ON`) · **Branch:** create `feat/ai-trading-substrate-phaseB` off `main` before Task 1.

---

## Verified facts (grounded — trust these)
- **Async→sync bridge** `ThreadHelper.h::run_async_wait(QObject* target, Start&& start)`: the `start` lambda receives a `signal_done` callback and runs on the adapter's thread; the caller (worker) blocks until `signal_done`. **CRITICAL — PM adapter signals are BROADCAST, not request-scoped:** the same `order_book_ready`/`market_detail_ready`/`error_occurred` fire for WebSocket pushes, retries, and other in-flight fetches, so a naive connect→`signal_done` completes early with the WRONG/empty payload against the LIVE adapter (a fake adapter emitting one signal in isolation HIDES this — the test/reality gap). Therefore the `start` lambda MUST: (1) connect the matching `*_ready` and in the slot **accept only if the payload id matches the request** (`book.asset_id==requested`; `market.market_id==requested`; for `search_results_ready`/`markets_ready` there's no id → first result is fine), else IGNORE and keep waiting; (2) connect `error_occurred` → error flag + `signal_done` (terminal); (3) **MANDATORY timeout** — `run_async_wait` has NO built-in timeout (bare `while(!done) cv.wait()`), so a never-matching fetch would wedge the single daemon worker PERMANENTLY for all later calls → create `QTimer::singleShot(15000,…)` INSIDE the `start` lambda (lives on the adapter thread) that sets a timeout flag + `signal_done`; (4) THEN call the fetch. Disconnect after (or `Qt::SingleShotConnection`). Timeout/error → the tool returns a clean failure, never a hang or stale data. (`get_quote` works one-shot under both `serve` and `--headless`, proving the main-thread event loop delivers these — but VERIFY a PM read one-shot early in T2, see below.)
- **Adapter** (`src/services/prediction/PredictionExchangeAdapter.h`, namespace `openmarketterminal::services::prediction`): pure-virtual `QObject`. Fetch methods: `list_markets(category, sort_by, limit, offset)`, `search(query, limit)`, `fetch_market(const MarketKey&)`, `fetch_order_book(const QString& asset_id)`. Signals: `markets_ready(QVector<PredictionMarket>)`, `search_results_ready(QVector<PredictionMarket>, QVector<PredictionEvent>)`, `market_detail_ready(PredictionMarket)`, `order_book_ready(PredictionOrderBook)`, `error_occurred(QString context, QString message)`. (The live trading methods `place_order`/`cancel_order` are NOT used in Phase B — paper only.)
- **Registry** `PredictionExchangeRegistry::instance().adapter(const QString& id)` → `PredictionExchangeAdapter*` (or nullptr); `available_ids()`; `register_adapter(std::unique_ptr<...>)`. Concrete adapters: `polymarket_ns::PolymarketAdapter` (`.../polymarket/PolymarketAdapter.h`), `kalshi_ns::KalshiAdapter` (`.../kalshi/KalshiAdapter.h`). **All PM adapter/registry sources are in `SERVICE_SOURCES` → compiled into `openterminal_core` with NO Widgets dependency**, so they are headless/daemon-linkable.
- **Adapter registration today** is GUI-only (`src/app/main.cpp:434-442`): `reg.register_adapter(std::make_unique<polymarket_ns::PolymarketAdapter>())`, same for `kalshi_ns::KalshiAdapter`. Phase B mirrors this in the headless runtime.
- **Types** (`src/services/prediction/PredictionTypes.h` — READ for exact field names before coding): `MarketKey{exchange_id, market_id, event_id, asset_ids}`; `PredictionMarket{question, description, category, end_date_iso, volume, liquidity, open_interest, tags, outcomes(QVector<Outcome>), …}`; `Outcome{name "Yes"/"No", asset_id, price 0–1}`; `PredictionOrderBook{asset_id, bids, asks (QVector<OrderLevel>), tick_size, min_order_size}`; `OrderLevel{price, size}`. (Confirm the market-id field name on `PredictionMarket` and the outcomes accessor.)
- **HeadlessRuntime bring-up** (`src/core/headless/HeadlessRuntime.cpp::init`): `register_all_migrations()` (l.42) → `Database::open` → `SecureStorage::init` → `CacheDatabase::open` → `register_all_data_services()` (l.68) → … → `register_core_tools()` (l.121). Insert PM adapter registration AFTER `register_all_data_services()` and BEFORE `register_core_tools()`.
- **Migrations:** last is `v049`; **next = `v050`**. Pattern = `v049_ai_trading.cpp` (`CREATE TABLE IF NOT EXISTS`, `register_migration_v050()` with `static bool done`, `MigrationRunner::register_migration({50,"pm_paper",apply_v050})`); declare in `MigrationRunner.h`, call in `RegisterAllMigrations.cpp`, add to `CMakeLists.txt` STORAGE list.
- **Phase A code to extend** (`src/mcp/tools/OrderFlowTools.cpp`): handlers for `prepare_order` (non-destructive) + `submit_order` (destructive); helpers `build_order_from_intent`, `risk_floor_check`/`RiskVerdict`, `read_cap`, `audit_prepare`/`audit_submit`, `reserve_for_submit`. The intent currently builds an equity `UnifiedOrder`; Phase B adds an `asset_class` discriminator at the TOP of each handler and routes prediction intents to new PM helpers. **Equity path (`asset_class` absent or `"equity"`) must stay byte-for-byte behaviorally unchanged.**
- **Gate helpers** (`src/mcp/tools/SettingsGate.{h,cpp}`): `cli_paper_trading_allowed()`, `cli_live_armed()`, `cli_trading_allowed()`, `is_gui_only_setting()` (the `cli.` prefix — so `cli.allowed_venues` + `cli.risk.*` are ALREADY GUI-only; no keystone change needed). `SettingsRepository::instance().get(key, default)`.
- **Repos** (`src/storage/repositories/`, `BaseRepository<T>` pattern; `db().execute(sql,params)` returns `Result<QSqlQuery>`): mirror `OrderDraftRepository`.
- **Tool idiom** (`src/mcp/tools/PaperTradingTools.cpp`): `ToolDef t; t.name; t.category; t.auth_required; t.is_destructive; t.input_schema = ToolSchemaBuilder()....build(); t.handler = [](const QJsonObject&)->ToolResult{...}; tools.push_back(std::move(t));` exposed via `std::vector<ToolDef> get_<group>_tools()`, registered in `McpInit.cpp::register_core_tools()`.

---

## File Structure
- Create: `src/storage/sqlite/migrations/v050_pm_paper.cpp`; `src/storage/repositories/PmPaperRepository.{h,cpp}` (positions + account); `src/mcp/tools/PredictionTools.{h,cpp}` (PM read tools); `src/mcp/tools/PmPaperEngine.{h,cpp}` (paper fill/mark engine).
- Modify: `src/mcp/tools/OrderFlowTools.cpp` (PM branches + PM risk floor); `src/mcp/tools/SettingsGate.{h,cpp}` (`cli_allowed_venues`/`cli_venue_allowed`); `src/mcp/McpInit.cpp` (register PredictionTools + the PM-paper tool); `src/core/headless/HeadlessRuntime.cpp` (register PM adapters); `src/screens/settings/SecuritySection.{h,cpp}` (allowed-venues field + max-exposure-per-topic); `src/storage/sqlite/migrations/MigrationRunner.h` + `RegisterAllMigrations.cpp`; `CMakeLists.txt` (core lib + GUI exe + STORAGE/MCP lists); `tests/CMakeLists.txt`.
- Tests: `tests/tst_pm_paper.cpp` (engine + storage + risk + flow), extend nothing in `tst_order_flow.cpp` except a no-regression assertion; `tests/e2e_pm_paper_trade.sh`.

---

## Task 1: Storage (v050) + venue/exposure constitution

**Files:** Create `src/storage/sqlite/migrations/v050_pm_paper.cpp`, `src/storage/repositories/PmPaperRepository.{h,cpp}`; Modify `MigrationRunner.h`, `RegisterAllMigrations.cpp`, `src/mcp/tools/SettingsGate.{h,cpp}`, `src/screens/settings/SecuritySection.{h,cpp}`, `CMakeLists.txt`; Test `tests/tst_pm_paper.cpp`.

- [ ] **Step 1: Migration v050** — `v050_pm_paper.cpp` (mirror `v049_ai_trading.cpp`):
  - `pm_paper_account(id INTEGER PRIMARY KEY CHECK(id=1), cash REAL NOT NULL DEFAULT 100000, currency TEXT NOT NULL DEFAULT 'USD', created_at TEXT NOT NULL DEFAULT '')` (single-row account).
  - `pm_paper_positions(id INTEGER PRIMARY KEY AUTOINCREMENT, venue TEXT NOT NULL, market_id TEXT NOT NULL, asset_id TEXT NOT NULL, outcome TEXT NOT NULL, category TEXT NOT NULL DEFAULT '', contracts REAL NOT NULL DEFAULT 0, avg_price REAL NOT NULL DEFAULT 0, cost_basis REAL NOT NULL DEFAULT 0, opened_at TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'open')`. **NO `UNIQUE(venue,asset_id)` constraint** — "one OPEN long per (venue,asset_id)" is enforced in the repo via `get_open` (filters `status='open'`), and closed rows remain as history. (A UNIQUE constraint would make a close→re-buy of the same asset throw on the next INSERT — the lifecycle bug; the repo-level invariant avoids it.)
  Declare `register_migration_v050()` in `MigrationRunner.h`; call it last in `register_all_migrations()`; add the .cpp to the STORAGE list in `CMakeLists.txt`.
- [ ] **Step 2: PmPaperRepository** (mirror `OrderDraftRepository`): account — `Result<double> cash()` (seeding the single row to 100000 on first read if absent), `Result<void> adjust_cash(double delta)`; positions — `Result<std::optional<PmPosition>> get_open(const QString& venue, const QString& asset_id)` (**filters `status='open'`** — this is the "one open per (venue,asset_id)" invariant, since there's no UNIQUE constraint and closed rows persist as history), `Result<qint64> insert_open(const PmPosition&)`, `Result<void> set_contracts(qint64 id, double contracts, double cost_basis, const QString& status)`, `Result<QVector<PmPosition>> list_open()`, and `Result<double> open_stake_in_category(const QString& category)` (SUM(cost_basis) WHERE status='open' AND category=?). `struct PmPosition{ qint64 id; QString venue, market_id, asset_id, outcome, category; double contracts, avg_price, cost_basis; QString opened_at, status; };`. (`buy_to_open` in T3 will `get_open`→UPDATE-if-present, else `insert_open` — so a close→re-buy of the same asset opens a fresh row instead of conflicting.) Add to STORAGE list.
- [ ] **Step 3: Venue gate helpers** — `SettingsGate.h`: `QStringList cli_allowed_venues();` (read `cli.allowed_venues`, default empty; split on `,`, trim, lowercase) and `bool cli_venue_allowed(const QString& venue);` (case-insensitive membership). `SettingsGate.cpp`: implement. (No keystone change — `cli.allowed_venues` + `cli.risk.max_exposure_per_topic` are already GUI-only via the `cli.` prefix.)
- [ ] **Step 4: Failing test** `tests/tst_pm_paper.cpp` (HeadlessRuntime+QTemporaryDir bring-up, mirror `tst_order_flow.cpp`; link `openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test`; register in `tests/CMakeLists.txt`): tables exist; account seeds to 100000 then `adjust_cash(-500)` → 99500; a position upserts + `get_open` round-trips + `open_stake_in_category` sums; `cli_venue_allowed` false by default, true after `SettingsRepository::set("cli.allowed_venues","polymarket,kalshi","cli")` for "polymarket"/"KALSHI", false for "foo". RED → implement → GREEN: `cmake --build /tmp/ot-build-test --target tst_pm_paper && ctest --test-dir /tmp/ot-build-test -R tst_pm_paper --output-on-failure`.
- [ ] **Step 5: GUI fields** — `SecuritySection`: an "Allowed AI venues (comma list)" text field → `cli.allowed_venues` and a "Max exposure per topic ($)" field → `cli.risk.max_exposure_per_topic`, persisted via `repo.set(..., "cli")`/`"cli"`-category, matching the existing toggle pattern (these are the ONLY way to set them).
- [ ] **Step 6: GUI build + selftest** `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal && .../OpenTerminal --selftest-tools; echo exit=$?` → 0.
- [ ] **Step 7: Commit** `feat(trading): PM paper storage (v050) + venue/exposure constitution` + Co-Authored-By.

---

## Task 2: PM read tools + headless adapter registration

**Files:** Create `src/mcp/tools/PredictionTools.{h,cpp}`; Modify `src/mcp/McpInit.cpp`, `src/core/headless/HeadlessRuntime.cpp`, `CMakeLists.txt`; Test extend `tests/tst_pm_paper.cpp`.

- [ ] **Step 1: Headless adapter registration** — in `HeadlessRuntime::init`, after `register_all_data_services()` and before `register_core_tools()`, mirror `main.cpp:434-438`:
```cpp
    {
        auto& reg = services::prediction::PredictionExchangeRegistry::instance();
        if (!reg.adapter("polymarket"))
            reg.register_adapter(std::make_unique<services::prediction::polymarket_ns::PolymarketAdapter>());
        if (!reg.adapter("kalshi"))
            reg.register_adapter(std::make_unique<services::prediction::kalshi_ns::KalshiAdapter>());
    }
```
(Include the registry + two adapter headers. The `if (!reg.adapter(...))` guard keeps it idempotent and lets a TEST pre-register a fake adapter for the same id and win.)
- [ ] **Step 2: PM read tools** in `PredictionTools.cpp` exposing `std::vector<ToolDef> get_prediction_tools()` (all `category "prediction"`, `auth_required None`, `is_destructive=false`). Each resolves `auto* a = PredictionExchangeRegistry::instance().adapter(venue)` (fail if null or venue empty) and uses `run_async_wait` (connect the matching `*_ready` + `error_occurred`, then call the fetch):
  - `pm_search_markets{venue, query, limit?}` → `search_results_ready` → return markets [{market_id, question, category, end_date_iso, liquidity, volume, outcomes:[{name, asset_id, price}]}].
  - `pm_get_market{venue, market_id}` → `fetch_market(MarketKey{exchange_id=venue, market_id=...})` → `market_detail_ready` → same market shape.
  - `pm_get_order_book{venue, asset_id}` → `fetch_order_book(asset_id)` → `order_book_ready` → {asset_id, best_bid, best_ask, spread, bids:[…], asks:[…], tick_size}.
  - `pm_list_markets{venue, category?, sort_by?, limit?}` → `list_markets(...)` → `markets_ready`.
  Register `get_prediction_tools()` in `McpInit.cpp::register_core_tools()` (`#include "mcp/tools/PredictionTools.h"`). Add `PredictionTools.cpp` to `CMakeLists.txt` (MCP/core list AND GUI exe list, like `OrderFlowTools.cpp`).
  **REVIEW INVARIANT (grep your own PM code):** no PM read tool — and no PM path anywhere — may call `adapter->place_order(...)` or `adapter->cancel_order(...)`. Those are the LIVE trading methods on the same adapter; they place REAL orders if credentials exist. PM reads call ONLY `list_markets/search/fetch_market/fetch_order_book`. `grep -nE "place_order|cancel_order" src/mcp/tools/PredictionTools.cpp src/mcp/tools/PmPaperEngine.cpp` must return NOTHING.
- [ ] **Step 2b: VERIFY the bridge works in headless ONE-SHOT (not just `serve`)** — before hardening tests, run one manual call: `cmake --build /tmp/ot-build-ht --target openterminalcli && /tmp/ot-build-ht/openterminalcli --headless mcp call pm_get_order_book '{"venue":"polymarket","asset_id":"<any>"}'; echo "rc=$?"`. It must RETURN (a clean error/empty on no-network is fine) and NOT hang — proving the one-shot main thread spins an event loop while the worker blocks (it should, since cold `get_quote` works one-shot). If it hangs, STOP and report — PM reads would only work under the daemon and the T6 headless step is invalid.
- [ ] **Step 3: Test (fake adapter seam)** — extend `tst_pm_paper.cpp`: define a `FakePredictionAdapter : public PredictionExchangeAdapter` whose `search`/`fetch_market`/`fetch_order_book` emit fixture `search_results_ready`/`market_detail_ready`/`order_book_ready` (via `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)` so the signal fires on the event loop while `run_async_wait` blocks the caller; emit with the REQUESTED asset_id/market_id so the new id-correlation accepts it). `id()` returns `"polymarket"`. **Register the fake in `initTestCase` BEFORE the first `rt_.init()`** (the `if(!reg.adapter("polymarket"))` headless guard means the real adapter wins if it's already in — so the fake must be registered first; the registry is a process-global singleton). Assert `pm_search_markets`/`pm_get_market`/`pm_get_order_book` return the fixture fields (best_ask/best_bid computed from the fixture book). Add a slot where the fake ALSO emits an `order_book_ready` for a DIFFERENT asset_id before the matching one → the tool must still return the CORRECT (requested) book (proves id-correlation). RED → implement → GREEN.
- [ ] **Step 4: Build + selftest** GUI `--selftest-tools` exit 0 (tools register cleanly); `cmake --build /tmp/ot-build-test` full suite green.
- [ ] **Step 5: Commit** `feat(trading): PM read tools + headless PM adapter registration` + Co-Authored-By.

---

## Task 3: PM paper engine + pm_paper_portfolio

**Files:** Create `src/mcp/tools/PmPaperEngine.{h,cpp}`; Modify `src/mcp/tools/PredictionTools.cpp` (add `pm_paper_portfolio`), `CMakeLists.txt`; Test extend `tests/tst_pm_paper.cpp`.

- [ ] **Step 1: PmPaperEngine** (free functions or a small struct over `PmPaperRepository`):
  - `struct PmFill{ bool ok; QString reason; QString action; double contracts; double fill_price; double cash_after; };`
  - `PmFill buy_to_open(venue, market_id, asset_id, outcome, category, contracts, fill_price)`: debit `contracts*fill_price` from cash (reject "insufficient paper cash" if cash would go negative); then `get_open(venue,asset_id)` → if an OPEN position exists, UPDATE it (weighted-avg the price, add contracts + cost) via `set_contracts`; else `insert_open` a fresh row. (A close→re-buy of the same asset thus opens a NEW row — no UNIQUE conflict, since there is no UNIQUE constraint and closed rows persist.) Returns ok with the fill.
  - `PmFill sell_to_close(venue, asset_id, contracts, fill_price)`: load the open position; reject "no open position to sell; short-open is not enabled in Phase B" if none; reject "cannot sell more than held (held N)" if `contracts > held`; reduce contracts + cost_basis pro-rata, credit `contracts*fill_price` to cash; if contracts hit 0 set status 'closed'. (**SELL-to-close ONLY** — never opens a short.)
  - `mark_to_market(const PmPosition&, double current_price) -> double` = `(current_price - avg_price) * contracts`.
- [ ] **Step 2: `pm_paper_portfolio` read tool** in `PredictionTools.cpp` (auth None, non-destructive): return `{cash, positions:[{venue, market_id, asset_id, outcome, contracts, avg_price, cost_basis, current_price?, unrealized_pnl?}], total_unrealized}`. Resolve `current_price` per position via `fetch_order_book` mid/last through the adapter when available; if not, omit current_price/pnl for that row (don't fail the whole call).
- [ ] **Step 3: Test** — extend `tst_pm_paper.cpp`: `buy_to_open(100 @ 0.60)` → cash 100000-60=99940, position 100@0.60, cost 60; `buy_to_open` same asset (50 @ 0.80) → contracts 150, avg ≈ (60+40)/150=0.667, cash 99900; `sell_to_close(60 @ 0.70)` → contracts 90, cash 99900+42=99942; `sell_to_close(90 @ …)` → contracts 0, position `status='closed'`; **then `buy_to_open` the SAME asset again (50 @ 0.55) → succeeds, a NEW open row exists (close→re-buy lifecycle, no UNIQUE conflict)**; `sell_to_close(1000 @ …)` → rejected (more than held); `sell_to_close` on an unheld asset → rejected ("short-open is not enabled"); `mark_to_market` math at a given current price. RED → implement → GREEN. Add `PmPaperEngine.cpp` to CMake (core + GUI). **Grep:** `PmPaperEngine.cpp` must contain NO `place_order`/`cancel_order` adapter call (paper fills are pure DB writes).
- [ ] **Step 4: Commit** `feat(trading): PM paper engine (buy-to-open/sell-to-close) + pm_paper_portfolio` + Co-Authored-By.

---

## Task 4: prepare_order PM branch + deterministic PM risk floor

**Files:** Modify `src/mcp/tools/OrderFlowTools.cpp`; Test extend `tests/tst_pm_paper.cpp`.

- [ ] **Step 1: asset_class discriminator** — at the TOP of BOTH the `prepare_order` and `submit_order` handlers, read `const QString asset_class = args.value("asset_class").toString("equity").trimmed().toLower();` (for submit, the asset_class comes from the loaded draft's intent_json). If `asset_class == "prediction"` → route to the new PM helpers (below). Else → the existing equity path UNCHANGED. Extend the `prepare_order` schema with optional PM fields (`asset_class`, `venue`, `market_id`, `asset_id`, `outcome`, `contracts`, `limit_price` already exists) — keep all equity fields optional too so one schema serves both; validate per class in the handler.
- [ ] **Step 2: PM order resolution + risk floor** (new helpers in OrderFlowTools.cpp anonymous namespace):
  - `pm_resolve_price(venue, asset_id, side) -> {bool ok; double price; double best_bid; double best_ask;}` via `run_async_wait` + `fetch_order_book` (BUY→best_ask, SELL→best_bid). No book/price → `ok=false`.
  - `pm_risk_floor_check(venue, market, asset_id, side, contracts, fill_price, best_bid, best_ask) -> RiskVerdict` (extend `RiskVerdict` or a PM sibling). `stake = contracts * fill_price`. Reject if:
    - `stake > read_cap("cli.risk.max_order_value", 25000)` ("exceeds max order value");
    - `contracts > read_cap("cli.risk.max_position_qty", 10000)` ("exceeds max position size");
    - `open_stake_in_category(market.category) + stake > read_cap("cli.risk.max_exposure_per_topic", 10000)` ("exceeds max exposure for topic '<cat>'");
    - `market.liquidity < read_cap("cli.risk.pm_min_liquidity", 1000)` ("market too illiquid");
    - `(best_ask - best_bid) > read_cap("cli.risk.pm_max_spread", 0.10)` ("spread too wide");
    - hours-to-resolution from `market.end_date_iso` `< read_cap("cli.risk.pm_min_hours_to_resolution", 1)` ("too close to resolution").
    - `max_loss` = `stake` (BUY long can lose its stake). Record in verdict + audit.
    All caps GUI-only `cli.risk.*` with finite defaults (empty/≤0 → default).
- [ ] **Step 3: prepare_order PM path** — validate (`cli_venue_allowed(venue)` else reject "venue '<v>' not in allowed venues — enable in GUI Settings"; `contracts>0`; `limit_price∈[0,1]` for limit; `asset_id`/`outcome` present; **side must be buy, OR sell only if an open position exists** — fetch `PmPaperRepository::get_open(venue, asset_id)`, reject SELL with no position "short-open is not enabled in Phase B"); fetch the market (`fetch_market` for category/liquidity/end_date); resolve price; run `pm_risk_floor_check`; on pass draft (`intent_json` carries `asset_class:"prediction"`+all PM fields, `account="pm-paper"`, `mode_hint="paper"`) + audit "prepare"/"prepared"; on fail audit "rejected" + `ok_data{status:"rejected",reason,checks}`.
- [ ] **Step 4: Test** — extend `tst_pm_paper.cpp` (with the fake adapter providing market + order book, and `cli.allowed_venues=polymarket`): valid BUY intent within caps → `prepared` + draft row + audit; oversized stake → rejected (max order value); over-exposure (pre-seed a position in the category near the cap) → rejected; illiquid market fixture → rejected; wide-spread book fixture → rejected; near-resolution end_date → rejected; venue not allowed → rejected; SELL with no position → rejected ("short-open"). RED → implement → GREEN. Full suite green.
- [ ] **Step 5: Commit** `feat(trading): prepare_order prediction branch + deterministic PM risk floor` + Co-Authored-By.

---

## Task 5: submit_order PM branch (paper executes, live hard-off)

**Files:** Modify `src/mcp/tools/OrderFlowTools.cpp`; Test extend `tests/tst_pm_paper.cpp`.

- [ ] **Step 1: submit_order PM path** — when the loaded draft's `asset_class=="prediction"`: re-parse the PM intent, re-fetch market + re-resolve price, **RE-RUN `pm_risk_floor_check` fresh** (revocable), then gate by mode:
  - `mode=="paper"`: require `cli_paper_trading_allowed()` (else reject "paper trading disabled — enable in GUI Settings") AND `cli_venue_allowed(venue)` (else "venue '<v>' not in allowed venues"); `reserve_for_submit(draft_id)` (atomic) — on loss reject "draft already used or not reservable"; execute on the PM paper engine — BUY → `buy_to_open(...)`, SELL → `sell_to_close(...)` (its own guards); map the `PmFill` → result; `update_status(draft_id, fill.ok ? "submitted" : "submit_failed")`; audit "submit" (decision `filled`/`rejected`, reason, risk snapshot, mode "paper", venue). Return `ok_data{status: fill.ok?"filled":"rejected", contracts, fill_price, cash_after, mode:"paper"}`.
  - `mode=="live"`: **HARD-OFF** → `ok_data{status:"rejected", reason:"live trading disabled (paper-first; not yet enabled)", mode:"live"}` + audit "denied". ZERO adapter `place_order` calls.
- [ ] **Step 2: Test** — extend `tst_pm_paper.cpp` (fake adapter, `cli.allow_paper_trading=true`, `cli.allowed_venues=polymarket`): prepare a valid BUY → submit paper → `filled`, a `pm_paper_positions` row exists, cash debited, draft "submitted", audit "submit"/paper; prepare another, set `cli.allow_paper_trading=false` → submit → rejected "paper trading disabled", no position; remove venue from `cli.allowed_venues` → submit → rejected venue; `mode:"live"` → rejected "live trading disabled", no position; SELL-to-close happy path (pre-open a position, prepare SELL, submit) → position reduced; revocable: lower `cli.risk.max_order_value` below the staked value between prepare and submit → submit rejected. RED → implement → GREEN. Full suite green.
- [ ] **Step 3: Build CLI+GUI + selftest** `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli` clean; `--selftest-tools` exit 0.
- [ ] **Step 4: Commit** `feat(trading): submit_order prediction paper (buy-to-open/sell-to-close) + live hard-off` + Co-Authored-By.

---

## Task 6: e2e + full regression + acceptance matrix

**Files:** Create `tests/e2e_pm_paper_trade.sh`.

- [ ] **Step 1: e2e** `tests/e2e_pm_paper_trade.sh` (mirror `tests/e2e_paper_trade.sh`'s cleanup-trap/fixed-profile/watchdog idioms; throwaway profile; NO GUI). The daemon uses REAL adapters (live PM network), so HARD-assert only the network-independent constitution and treat live-data steps as best-effort:
  - Bootstrap profile DB; `sqlite3` write `cli.allow_paper_trading=true` AND `cli.allowed_venues=polymarket,kalshi` (simulated GUI). Start `serve`; assert `kind=daemon`.
  - **HARD asserts (deterministic, no live data needed):** `prepare_order {asset_class:"prediction", venue:"unlisted", …}` → rejected "not in allowed venues"; `submit_order {…, mode:"live"}` on any draft id → rejected "live trading disabled"; `set_setting cli.allowed_venues '…'` over the daemon → denied (exit 5, keystone); a non-prediction destructive tool (`live_place_order`) → denied (carve-out still scoped).
  - **Best-effort (live network, watchdog, accept rc 0/5):** `pm_search_markets {venue:"polymarket", query:"…"}` then `prepare_order`/`submit_order paper` against a returned market — assert no hang/crash; if it fills, assert a portfolio position. Document that the authoritative fill proof is the unit test (fake adapter).
  - `serve --stop` (grace + bridge.json removed). Leak-proof cleanup.
- [ ] **Step 2: Run it; paste output** (`bash tests/e2e_pm_paper_trade.sh; echo rc=$?`).
- [ ] **Step 3: Full regression (paste):** `ctest --test-dir /tmp/ot-build-test --output-on-failure` (all tst_*, incl `tst_order_flow` UNCHANGED + new `tst_pm_paper`); `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`; every `--selftest-*` exits 0 (enumerate as in Phase A); headless one-shot `--headless mcp list | head -c 80` + a headless `pm_search_markets` (best-effort). Re-run `bash tests/e2e_paper_trade.sh` (equity) → rc 0 (NO regression to Phase A).
- [ ] **Step 4: Map to the acceptance matrix (report each PASS/FAIL + where proven):** (1) PM paper happy path both venues — unit (fake adapter, both ids) + e2e best-effort; (2) PM risk floor (exposure/liquidity/spread/proximity/order-value) at prepare + submit re-check — unit; (3) gate ladder paper-off→denied, live→disabled, venue-not-allowed→denied — unit + e2e; (4) constitution: `cli.allowed_venues`/`cli.risk.max_exposure_per_topic` refused via set_setting even with settings-write on — unit + e2e; (5) buy-to-open + sell-to-close only (short-open rejected) — unit; (6) audit completeness — unit; (7) equity Phase A unchanged — `tst_order_flow` + equity e2e; (8) no-regression selftests/headless.
- [ ] **Step 5: Commit** `test(trading): PM paper e2e + Phase-B acceptance matrix` + Co-Authored-By.

---

## Self-Review
**Spec coverage:** read tools (T2) ✓; PM paper engine + storage (T1/T3) ✓; unified prepare/submit PM branch (T4/T5) ✓; PM risk floor exposure/liquidity/spread/proximity/max-loss (T4) ✓; both venues (T2 registration + venue-agnostic engine; tests use a fake adapter id, e2e lists both) ✓; best-available immediate fill (T3) ✓; mark-to-market only (T3; no settlement) ✓; BUY-to-open + SELL-to-close only (T3/T4/T5) ✓; constitution `cli.allowed_venues`+`cli.risk.max_exposure_per_topic` GUI-only (free via `cli.` prefix; readers T1) ✓; headless adapter registration (T2) ✓; live hard-off (T5) ✓; equity unchanged (T4 discriminator default "equity"; T6 regression) ✓.

**Type consistency:** `PmPosition`/`PmPaperRepository` (T1) used by `PmPaperEngine` (T3) + risk floor (T4) + submit (T5); `get_prediction_tools` (T2) registered once; `pm_resolve_price`/`pm_risk_floor_check` (T4) reused in submit (T5); `asset_class` discriminator consistent prepare↔submit; adapter signal/fetch names match the verified list.

**Placeholder scan:** code-shaped steps carry real SQL/signatures/signal names; the few "confirm the exact field name" notes (PredictionMarket market-id/outcomes accessor; whether run_async_wait has a built-in timeout) are grep-grounded with a stated fallback, not vague.

**SAFETY NOTE for executors (the catastrophic invariant first):** the PM paper engine and all PM paths must NEVER call `adapter->place_order(...)` / `adapter->cancel_order(...)` — those are the adapter's LIVE trading methods sitting on the very adapter you call `fetch_order_book` on, and they place REAL orders if credentials exist. Paper fills are PURE DB writes against fetched prices. Every PM task's review MUST run `grep -nE "place_order|cancel_order" src/mcp/tools/PredictionTools.cpp src/mcp/tools/PmPaperEngine.cpp src/mcp/tools/OrderFlowTools.cpp` and confirm the PM paths contain none. Also: `submit_order … mode:"live"` for prediction MUST be a string rejection with zero adapter calls; never expose the adapter's live trading methods as carved-out tools; never make `cli.allowed_venues` / `cli.risk.*` CLI-writable (GUI-only via the prefix — don't add an exception). Keep the equity Phase A path behaviorally unchanged.
