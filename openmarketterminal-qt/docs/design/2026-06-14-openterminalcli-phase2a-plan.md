# openterminalcli Phase 2a (Headless Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extract an `openterminal_core` static lib (Qt6::Core/Network/Sql, no Widgets) and give `openterminalcli` a one-shot `--headless` mode that runs `quote`/`hub`/`selftest` in-process with no GUI — the foundation every later headless tool reuses.

**Architecture:** Decouple `DataHub` from QtWidgets via an injected hook; split `McpInit` into core/gui tool registration; move the non-GUI source groups into `openterminal_core` linked by both the GUI exe and the CLI; a `HeadlessRuntime` brings up DB→SecureStorage→DataHub→core tools under `QCoreApplication`; `--headless` routes the existing command tree through it. The GUI must build and pass all `--selftest-*` unchanged (the no-regression gate).

**Tech Stack:** C++20, Qt6 (Core/Network/Sql/Widgets), CMake+Ninja unity build, QtTest (`-DOPENMARKETTERMINAL_BUILD_TESTS=ON`).

**Spec:** `openmarketterminal-qt/docs/design/2026-06-14-openterminalcli-phase2-headless-design.md`
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · **GUI build:** `/tmp/ot-build-ht` · **Test build:** `/tmp/ot-build-test`
**Branch:** create `feat/openterminalcli-phase2a` off `main` before Task 1.

**Scope note:** This is Plan **2a** (foundation + `quote`/`hub`/`selftest` slice). Plan **2b** (widen to the full read+write data-tool set, per-service headless vetting, wiring `cli.allow_settings_write` to all settings-write tools) is a separate later plan; 2a builds the gate *mechanism* and proves it on the trading tier.

---

## Verified facts
- `DataHub::is_owner_active_for_work(QObject*)` (`src/datahub/DataHub.cpp:42`) is the ONLY QtWidgets use in DataHub; returns `true` for non-widget owners; 2 callers (`:563,:571`). `DataHub.h:43` `class DataHub : public QObject` already includes `<functional>`.
- `McpInit::initialize_all_tools()` (`src/mcp/McpInit.cpp`) is a flat list of `provider.register_tools(tools::get_<X>_tools())`.
- Init in `main.cpp`: `Database::instance().open(db_path)` (runs migrations internally — no separate MigrationRunner call) → `SecureStorage::instance().init()` → `CacheDatabase::instance().open(cache_path)`.
- GUI target (`CMakeLists.txt:2477`) lists source-group vars: `CORE/NETWORK/DATAHUB/AUTH/STORAGE/PYTHON/LLM_SERVICE/AI_CHAT_SCREEN/MCP/TRADING/ALGO_ENGINE/SERVICE/UI/SCREEN/APP`. `target_link_libraries(OpenMarketTerminal PRIVATE Qt6::Widgets …)` at `:2831`.
- GUI-coupled files that must stay in the GUI exe (NOT the core lib): CORE → `layout/WorkspaceShell.cpp`, `layout/DockLayoutSelftest.cpp`, `symbol/SymbolDragSource.cpp`, `actions/builtin_actions.cpp`, `keys/WindowCycler.cpp`; AUTH → `lock/LockOverlayController.cpp`; SERVICE → `workflow/adapters/ServiceBridges.cpp`; MCP tool files → `DataHubPeekHelpers.cpp`, `NavigationTools.cpp`, `DashboardTools.cpp`, `ExcelTools.cpp`, `WorkspaceTools.cpp`, `WorkspaceTools_MonitorsWindows.cpp`, `WorkspaceTools_Panels.cpp`, `WorkspaceTools_LayoutsSnapshots.cpp`, `WorkspaceTools_SymbolsActions.cpp`. (Each must be confirmed by `grep -lE '#include <Q(Widget|Application|...)>'` during Task 3 — the linker is the final arbiter.)

---

## File Structure
- Modify: `src/datahub/DataHub.h`/`.cpp` (hook), `src/app/main.cpp` (install GUI hook; call `register_gui_tools`), `src/mcp/McpInit.h`/`.cpp` (split).
- Create: `src/core/headless/HeadlessRuntime.h`/`.cpp` (in core lib).
- Modify: `src/cli/CommandDispatch.{h,cpp}`, `src/cli/main.cpp` (--headless route).
- Create: `src/mcp/tools/SettingsGate.h`/`.cpp` (shared `cli.allow_*` checks) — used by headless gate + bridge.
- Modify: `src/mcp/TerminalMcpBridge.cpp` (conditional destructive_token), `src/mcp/ToolConfirmationGate` wiring in CLI, GUI Settings screen (two toggles).
- Modify: `CMakeLists.txt` (new `openterminal_core` lib; repartition groups; CLI links lib), `tests/CMakeLists.txt` + new `tests/tst_*`.

---

## Task 1: Decouple DataHub from QtWidgets (injected owner-active hook)

**Files:** Modify `src/datahub/DataHub.h`, `src/datahub/DataHub.cpp`, `src/app/main.cpp`; Test `tests/tst_datahub_hook.cpp`.

- [ ] **Step 1: Write the failing test** — `tests/tst_datahub_hook.cpp`:
```cpp
#include <QtTest>
#include <QObject>
#include "datahub/DataHub.h"
using namespace openmarketterminal;
class TstDataHubHook : public QObject {
    Q_OBJECT
private slots:
    void default_hook_treats_all_owners_active() {
        DataHub::set_owner_active_hook(nullptr); // reset to default
        QObject o;
        QVERIFY(DataHub::owner_active_for_test(&o)); // default → true
    }
    void installed_hook_is_honored() {
        DataHub::set_owner_active_hook([](QObject*){ return false; });
        QObject o;
        QVERIFY(!DataHub::owner_active_for_test(&o));
        DataHub::set_owner_active_hook(nullptr);
    }
};
QTEST_MAIN(TstDataHubHook)
#include "tst_datahub_hook.moc"
```
Add target to `tests/CMakeLists.txt`:
```cmake
add_executable(tst_datahub_hook tst_datahub_hook.cpp
    ${CMAKE_SOURCE_DIR}/src/datahub/DataHub.cpp
    ${CMAKE_SOURCE_DIR}/src/datahub/DataHubMetaTypes.cpp)
target_include_directories(tst_datahub_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_datahub_hook PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_datahub_hook COMMAND tst_datahub_hook)
```

- [ ] **Step 2: Run — verify it FAILS** (`set_owner_active_hook`/`owner_active_for_test` undefined; and the target links `Qt6::Core` only, so if DataHub still `#include <QWidget>` it fails to link → proves the decoupling is needed):
```bash
cmake --build /tmp/ot-build-test --target tst_datahub_hook
```
Expected: FAIL (undefined symbols and/or QtWidgets link error).

- [ ] **Step 3: Add the hook to `DataHub.h`** — in the `public:` block:
```cpp
    /// Visibility/work-gating hook. The GUI installs a version that walks the
    /// owner's top-level QWidget; headless leaves the default (all owners active).
    /// Passing nullptr resets to the default.
    static void set_owner_active_hook(std::function<bool(QObject*)> hook);
    /// Test-only accessor for the resolved hook decision.
    static bool owner_active_for_test(QObject* owner);
```

- [ ] **Step 4: Implement in `DataHub.cpp`** — replace the QWidget logic. Remove `#include <QWidget>` (and any other Widgets includes). At file scope (anon namespace), replace `is_owner_active_for_work`:
```cpp
namespace {
std::function<bool(QObject*)> g_owner_active_hook; // empty = default (all active)
bool is_owner_active_for_work(QObject* owner) {
    if (!owner) return true;
    if (g_owner_active_hook) return g_owner_active_hook(owner);
    return true; // headless / no hook: never pause for visibility
}
} // namespace

void DataHub::set_owner_active_hook(std::function<bool(QObject*)> hook) {
    g_owner_active_hook = std::move(hook);
}
bool DataHub::owner_active_for_test(QObject* owner) { return is_owner_active_for_work(owner); }
```
(The two existing callers at the old `:563,:571` keep calling `is_owner_active_for_work` — unchanged.)

- [ ] **Step 5: Install the QWidget hook in the GUI** — `src/app/main.cpp`, right after `QApplication app(argc, argv);` (~line 240), add:
```cpp
    // Restore widget-visibility work-gating for the GUI (headless leaves the
    // default all-active hook). Walks the owner to its top-level window.
    openmarketterminal::DataHub::set_owner_active_hook([](QObject* owner) -> bool {
        auto* w = qobject_cast<QWidget*>(owner);
        if (!w) return true;
        QWidget* top = w->window();
        return !top || top->isVisible();
    });
```
Add `#include <QWidget>` to main.cpp if not present (it is a GUI TU, Widgets is fine here) and ensure `#include "datahub/DataHub.h"`.

- [ ] **Step 6: Run test — PASS**, and confirm DataHub now links under Core-only:
```bash
cmake --build /tmp/ot-build-test --target tst_datahub_hook && ctest --test-dir /tmp/ot-build-test -R tst_datahub_hook --output-on-failure
```
Expected: PASS (which also proves `DataHub.cpp` no longer needs QtWidgets).

- [ ] **Step 7: Verify GUI still builds + visibility behavior intact:**
```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal
```
Expected: links clean.

- [ ] **Step 8: Commit**
```bash
git add src/datahub/DataHub.h src/datahub/DataHub.cpp src/app/main.cpp tests/tst_datahub_hook.cpp tests/CMakeLists.txt
git commit -m "refactor(datahub): decouple from QtWidgets via injected owner-active hook"
```

---

## Task 2: Split `McpInit` into `register_core_tools()` / `register_gui_tools()`

**Files:** Modify `src/mcp/McpInit.h`, `src/mcp/McpInit.cpp`, `src/app/main.cpp`; Test `tests/tst_mcp_init_split.cpp`.

- [ ] **Step 1: Failing test** — `tests/tst_mcp_init_split.cpp`:
```cpp
#include <QtTest>
#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
using namespace openmarketterminal::mcp;
class TstMcpInitSplit : public QObject {
    Q_OBJECT
private slots:
    void core_registers_data_tools_not_gui() {
        register_core_tools();
        auto& p = McpProvider::instance();
        QVERIFY(p.has_tool("get_quote"));        // data tool present
        QVERIFY(!p.has_tool("navigate"));        // GUI tool absent until register_gui_tools()
    }
};
QTEST_MAIN(TstMcpInitSplit)
#include "tst_mcp_init_split.moc"
```
(If `McpProvider` lacks a `has_tool(QString)` predicate, add a minimal one in this task: `bool has_tool(const QString&) const;` returning whether the catalog contains it. Confirm the real tool names first via `grep 't.name =' src/mcp/tools/MarketsTools.cpp src/mcp/tools/NavigationTools.cpp` — use the actual names, e.g. `get_quote` and whatever the navigation tool is called.)

- [ ] **Step 2: Run — FAIL** (`register_core_tools` undefined):
```bash
cmake --build /tmp/ot-build-test --target tst_mcp_init_split
```

- [ ] **Step 3: Declare the split in `McpInit.h`:**
```cpp
/// Register only the non-GUI ("core") tools — safe in a headless process.
void register_core_tools();
/// Register the GUI-only tools (navigation, dashboard, workspace, excel, ai-chat,
/// file-manager). Requires a running GUI; call only from the GUI app.
void register_gui_tools();
```
(Keep `initialize_all_tools()` declared; it now = both.)

- [ ] **Step 4: Implement in `McpInit.cpp`** — move each `provider.register_tools(...)` line into the right function. GUI set = navigation, dashboard, workspace (all WorkspaceTools), excel, ai_chat, file_manager. Core set = everything else (news, markets, watchlist, portfolio, notes, agentic_memory, crypto_trading, paper_trading, live_trading, edgar, ma_analytics, alt_investments, data_sources, profile, report_builder, settings, python, system, datahub, mcp_servers, quant_lab, agents, dbnomics, gov_data, equity_research, geopolitics, surface_analytics, meta). Keep the `#include`s for GUI-tool headers; `register_gui_tools()` references them so those .cpp stay in the GUI build (Task 3 keeps them out of the lib).
```cpp
void register_core_tools() {
    auto& provider = McpProvider::instance();
    provider.register_tools(tools::get_news_tools());
    provider.register_tools(tools::get_markets_tools());
    /* …all non-GUI modules… */
    provider.register_tools(tools::get_meta_tools());
}
void register_gui_tools() {
    auto& provider = McpProvider::instance();
    provider.register_tools(tools::get_navigation_tools());
    provider.register_tools(tools::get_dashboard_tools());
    provider.register_tools(tools::get_workspace_tools());
    provider.register_tools(tools::get_excel_tools());
    provider.register_tools(tools::get_ai_chat_tools());
    provider.register_tools(tools::get_file_manager_tools());
}
void initialize_all_tools() { register_core_tools(); register_gui_tools(); /* + external server start as before */ }
```
(Preserve any non-tool init that `initialize_all_tools()` did after registration, e.g. external MCP server startup — keep it in `initialize_all_tools()` after the two calls.)

- [ ] **Step 5: Run test — PASS:**
```bash
cmake --build /tmp/ot-build-test --target tst_mcp_init_split && ctest --test-dir /tmp/ot-build-test -R tst_mcp_init_split --output-on-failure
```

- [ ] **Step 6: GUI still builds** (`initialize_all_tools()` unchanged externally):
```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal
```

- [ ] **Step 7: Commit**
```bash
git add src/mcp/McpInit.h src/mcp/McpInit.cpp src/mcp/McpProvider.h src/mcp/McpProvider.cpp tests/tst_mcp_init_split.cpp tests/CMakeLists.txt
git commit -m "refactor(mcp): split tool registration into core vs gui"
```

---

## Task 3: Extract the `openterminal_core` static library (CMake)

**Files:** Modify `CMakeLists.txt`.

This repartitions the GUI target's source groups: the GUI-coupled files move into new `*_GUI_SOURCES` vars; `openterminal_core` aggregates the now-core-only groups; the GUI exe links the lib + the GUI vars.

- [ ] **Step 1: Carve the GUI-coupled files out of CORE/AUTH/MCP groups.** In `CMakeLists.txt`, after the `set(MCP_SOURCES …)` block, add:
```cmake
# GUI-coupled files pulled OUT of the core groups so openterminal_core stays
# Qt6::Core/Network/Sql-only. These compile into the GUI exe, not the lib.
set(CORE_GUI_SOURCES
    src/core/layout/WorkspaceShell.cpp
    src/core/layout/DockLayoutSelftest.cpp
    src/core/symbol/SymbolDragSource.cpp
    src/core/actions/builtin_actions.cpp
    src/core/keys/WindowCycler.cpp)
set(AUTH_GUI_SOURCES src/auth/lock/LockOverlayController.cpp)
set(SERVICE_GUI_SOURCES src/services/workflow/adapters/ServiceBridges.cpp)
set(MCP_GUI_SOURCES
    src/mcp/tools/DataHubPeekHelpers.cpp
    src/mcp/tools/NavigationTools.cpp
    src/mcp/tools/DashboardTools.cpp
    src/mcp/tools/ExcelTools.cpp
    src/mcp/tools/WorkspaceTools.cpp
    src/mcp/tools/WorkspaceTools_MonitorsWindows.cpp
    src/mcp/tools/WorkspaceTools_Panels.cpp
    src/mcp/tools/WorkspaceTools_LayoutsSnapshots.cpp
    src/mcp/tools/WorkspaceTools_SymbolsActions.cpp)
list(REMOVE_ITEM CORE_SOURCES ${CORE_GUI_SOURCES})
list(REMOVE_ITEM AUTH_SOURCES ${AUTH_GUI_SOURCES})
list(REMOVE_ITEM SERVICE_SOURCES ${SERVICE_GUI_SOURCES})
list(REMOVE_ITEM MCP_SOURCES ${MCP_GUI_SOURCES})
```

- [ ] **Step 2: Define the lib** (immediately before the `add_executable(OpenMarketTerminal …)` block, ~line 2475):
```cmake
add_library(openterminal_core STATIC
    ${CORE_SOURCES} ${NETWORK_SOURCES} ${DATAHUB_SOURCES} ${AUTH_SOURCES}
    ${STORAGE_SOURCES} ${PYTHON_SOURCES} ${LLM_SERVICE_SOURCES}
    ${MCP_SOURCES} ${TRADING_SOURCES} ${ALGO_ENGINE_SOURCES} ${SERVICE_SOURCES})
set_target_properties(openterminal_core PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(openterminal_core PUBLIC src)
target_link_libraries(openterminal_core PUBLIC
    Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent miniz openmarketterminal_ed25519)
if(HAS_QT_WEBSOCKETS)
    target_link_libraries(openterminal_core PUBLIC Qt6::WebSockets)
endif()
```
Then add (after the lib) any `target_compile_definitions` the moved sources need — mirror the GUI target's feature-flag defs that apply to core groups (e.g. `HAS_QT_WEBSOCKETS`, version strings). Carry the existing `SKIP_UNITY_BUILD_INCLUSION` `set_source_files_properties(... PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)` for files like `KeychainKey.cpp` so they still skip unity.

- [ ] **Step 3: Slim the GUI exe to link the lib + GUI-only groups.** Replace the `add_executable(OpenMarketTerminal …)` source list with:
```cmake
add_executable(OpenMarketTerminal ${_OPENMARKETTERMINAL_EXE_FLAGS}
    ${AI_CHAT_SCREEN_SOURCES}
    ${CORE_GUI_SOURCES} ${AUTH_GUI_SOURCES} ${SERVICE_GUI_SOURCES} ${MCP_GUI_SOURCES}
    ${UI_SOURCES} ${SCREEN_SOURCES} ${APP_SOURCES}
    ${_OPENMARKETTERMINAL_PLATFORM_SOURCES})
target_link_libraries(OpenMarketTerminal PRIVATE openterminal_core Qt6::Widgets)
```
(Keep the rest of the GUI target's existing `target_link_libraries`/`target_compile_definitions`/macOS framework lines below it — `openterminal_core` is added to the link set; `Qt6::Widgets` stays.)

- [ ] **Step 4: Configure + build the GUI — the NO-REGRESSION GATE:**
```bash
cmake -S . -B /tmp/ot-build-ht -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal 2>&1 | tail -20
```
Expected: configures and links clean. If the lib fails to link with an undefined QtWidgets symbol, the offending file is a missed GUI-coupled source → add it to the matching `*_GUI_SOURCES` and rerun. Iterate until the lib builds Core-only and the GUI links.

- [ ] **Step 5: Prove the GUI still behaves — run every selftest:**
```bash
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds dock-layout universe-scan paper portfolio-replication bridge-discovery; do
  echo "== --selftest-$t =="; "$APP" --selftest-$t; echo "exit=$?"; done
"$APP" --selftest-workflow-honesty; echo "exit=$?"
```
Expected: each prints OK and `exit=0` (same as before the extraction).

- [ ] **Step 6: Commit**
```bash
git add CMakeLists.txt
git commit -m "build: extract openterminal_core static lib (Qt6::Core/Network/Sql, no Widgets)"
```

---

## Task 4: `HeadlessRuntime` — in-process bring-up under QCoreApplication

**Files:** Create `src/core/headless/HeadlessRuntime.h`/`.cpp` (added to `CORE_SOURCES` → lands in the lib); Test `tests/tst_headless_runtime.cpp`.

- [ ] **Step 1: Header** — `src/core/headless/HeadlessRuntime.h`:
```cpp
#pragma once
#include "mcp/McpTypes.h"
#include <QJsonObject>
#include <QString>
namespace openmarketterminal::headless {
struct InitResult { bool ok = false; QString error; };
class HeadlessRuntime {
public:
    /// Brings up DB (+migrations) → SecureStorage → DataHub default hook →
    /// register_core_tools() → capability filter + gate presenter. profile
    /// selects the datadir (same rule as the GUI). Idempotent.
    InitResult init(const QString& profile);
    /// Dispatch one core tool synchronously (pumps the event loop until done).
    mcp::ToolResult call_tool(const QString& name, const QJsonObject& args);
    void shutdown();
};
} // namespace openmarketterminal::headless
```

- [ ] **Step 2: Failing test** — `tests/tst_headless_runtime.cpp`:
```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "core/headless/HeadlessRuntime.h"
using namespace openmarketterminal::headless;
class TstHeadlessRuntime : public QObject {
    Q_OBJECT
private slots:
    void init_brings_up_db_and_core_tools() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        HeadlessRuntime rt;
        auto r = rt.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
        // a pure-compute / no-network core tool round-trips (system/meta tool):
        auto res = rt.call_tool("get_terminal_context", {}); // pick a confirmed no-network core tool
        QVERIFY(res.success);
        rt.shutdown();
    }
};
QTEST_MAIN(TstHeadlessRuntime)
#include "tst_headless_runtime.moc"
```
(Pick a real no-network core tool for the assertion — `grep 't.name =' src/mcp/tools/SystemTools.cpp src/mcp/tools/MetaTools.cpp` and use one that returns success without network/services, e.g. a context/version/list tool. If none is purely local, assert on `get_quote` for `AAPL` and mark the test as network-dependent.) Add the target to `tests/CMakeLists.txt` linking `openterminal_core Qt6::Core Qt6::Test`.

- [ ] **Step 3: Run — FAIL** (HeadlessRuntime unimplemented):
```bash
cmake --build /tmp/ot-build-test --target tst_headless_runtime
```

- [ ] **Step 4: Implement `HeadlessRuntime.cpp`.** `init()` replicates the verified `main.cpp` sequence (headless-relevant subset) under the assumption a `QCoreApplication` already exists (the CLI's), then registers core tools:
```cpp
#include "core/headless/HeadlessRuntime.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/secure/SecureStorage.h"
#include "datahub/DataHub.h"
#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
#include "mcp/ToolConfirmationGate.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFutureWatcher>

namespace openmarketterminal::headless {

InitResult HeadlessRuntime::init(const QString& profile) {
    ProfileManager::instance().set_active(profile);
    const QString db_path = AppPaths::data() + "/openmarketterminal.db";
    auto db = Database::instance().open(db_path);          // runs migrations internally
    if (db.is_err()) return {false, QString::fromStdString(db.error())};
    SecureStorage::instance().init();                       // machine-derived key, no prompt
    CacheDatabase::instance().open(AppPaths::cache() + "/cache.db");
    // DataHub uses its default owner-active hook (all active) — correct headless.
    // Deny destructive by default; Task 6 makes this honor cli.allow_trading.
    mcp::ToolConfirmationGate::instance().set_presenter(
        [](const QString&, const QString&){ return false; });
    mcp::register_core_tools();
    return {true, {}};
}

mcp::ToolResult HeadlessRuntime::call_tool(const QString& name, const QJsonObject& args) {
    QFutureWatcher<mcp::ToolResult> w;
    QEventLoop loop;
    QObject::connect(&w, &QFutureWatcher<mcp::ToolResult>::finished, &loop, &QEventLoop::quit);
    w.setFuture(mcp::McpProvider::instance().call_tool_async(name, args));
    loop.exec();
    return w.future().resultCount() > 0 ? w.future().result()
                                        : mcp::ToolResult::fail("no result");
}

void HeadlessRuntime::shutdown() { /* singletons own their teardown; nothing to do for one-shot */ }

} // namespace openmarketterminal::headless
```
Add `src/core/headless/HeadlessRuntime.cpp` to `CORE_SOURCES` in `CMakeLists.txt`. (If `init()` for a tool that needs a service like `get_quote` requires that service to be registered with the hub, add the minimal `Service::instance().ensure_registered_with_hub()` calls here — determined by what the chosen test tool needs; bounded by the test passing.)

- [ ] **Step 5: Run — PASS:**
```bash
cmake --build /tmp/ot-build-test --target tst_headless_runtime && ctest --test-dir /tmp/ot-build-test -R tst_headless_runtime --output-on-failure
```

- [ ] **Step 6: Commit**
```bash
git add src/core/headless/HeadlessRuntime.h src/core/headless/HeadlessRuntime.cpp CMakeLists.txt tests/tst_headless_runtime.cpp tests/CMakeLists.txt
git commit -m "feat(headless): HeadlessRuntime brings up DB+DataHub+core tools under QCoreApplication"
```

---

## Task 5: Wire `openterminalcli --headless`

**Files:** Modify `src/cli/CommandDispatch.h`/`.cpp`, `src/cli/main.cpp`, `CMakeLists.txt` (CLI links `openterminal_core`).

- [ ] **Step 1: Add `--headless` to `GlobalOpts`** (`CommandDispatch.h`): add `bool headless = false;` and parse it in `parse_global_opts` (same loop as `--json`).

- [ ] **Step 2: Route commands through the runtime when headless.** In `CommandDispatch.cpp`, introduce a tiny indirection so `mcp`/`hub`/`quote` use either the `BridgeClient` (attach) or a `HeadlessRuntime` (in-process). Concretely, when `opts.headless`, replace `make_client(...)` resolution with a process-lifetime `HeadlessRuntime` initialized once:
```cpp
#include "core/headless/HeadlessRuntime.h"
// in dispatch(), before command handling:
static openmarketterminal::headless::HeadlessRuntime g_rt;
if (opts.headless) {
    auto ir = g_rt.init(opts.profile);
    if (!ir.ok) { std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error)); return 7; }
}
```
For each `mcp call`/`quote`/`hub` path, call `g_rt.call_tool(name, args)` (returning a `ToolResult` → reuse `emit_result` by adapting it to a `ToolResult` directly, since headless has no HTTP `ClientResult`). Add an `emit_result` overload taking `(const GlobalOpts&, const mcp::ToolResult&)` that maps: `success==false` → exit 5; else print `to_json()`. `mcp list`/`describe` in headless read `McpProvider::instance().get_all_tools()` directly instead of `GET /tools`.

- [ ] **Step 3: Link the lib into the CLI** — in `CMakeLists.txt`, the `openterminalcli` target: `target_link_libraries(openterminalcli PRIVATE openterminal_core Qt6::Core Qt6::Network)` (it already lists Core/Network; add `openterminal_core`).

- [ ] **Step 4: Build + headless smoke:**
```bash
cmake --build /tmp/ot-build-ht --target openterminalcli
/tmp/ot-build-ht/openterminalcli --headless version; echo "exit=$?"          # 0
/tmp/ot-build-ht/openterminalcli --headless mcp list | head -c 200; echo     # core catalog, no GUI
/tmp/ot-build-ht/openterminalcli --json --headless quote AAPL; echo "exit=$?" # live quote, 0
/tmp/ot-build-ht/openterminalcli --headless hub topics | head -c 200; echo
```
Expected: `mcp list` shows the core (non-GUI) catalog; `quote AAPL` returns a real quote with NO GUI running. (This is the headline Phase-2a proof.)

- [ ] **Step 5: Commit**
```bash
git add src/cli/CommandDispatch.h src/cli/CommandDispatch.cpp src/cli/main.cpp CMakeLists.txt
git commit -m "feat(cli): --headless runs commands in-process via HeadlessRuntime"
```

---

## Task 6: The two gate settings (cli.allow_settings_write, cli.allow_trading)

**Files:** Create `src/mcp/tools/SettingsGate.h`/`.cpp`; Modify `HeadlessRuntime.cpp`, `TerminalMcpBridge.cpp`, GUI Settings screen; Test `tests/tst_settings_gate.cpp`.

- [ ] **Step 1: Shared gate helpers** — `src/mcp/tools/SettingsGate.{h,cpp}` reading the settings DB:
```cpp
// SettingsGate.h
#pragma once
namespace openmarketterminal::mcp {
bool cli_trading_allowed();          // SettingsRepository "cli.allow_trading" == "true"
bool cli_settings_write_allowed();   // SettingsRepository "cli.allow_settings_write" == "true"
}
```
`.cpp` reads `SettingsRepository::instance().get("cli.allow_trading","false").value()=="true"` etc.

- [ ] **Step 2: Failing test** — `tests/tst_settings_gate.cpp`: with a temp HOME + DB, default both false; after writing the setting true, the helper returns true. (Open `Database` first so `SettingsRepository` works.)

- [ ] **Step 3: Headless enforcement** — in `HeadlessRuntime::init`, replace the always-deny presenter with one that honors the setting:
```cpp
    mcp::ToolConfirmationGate::instance().set_presenter(
        [](const QString&, const QString&){ return mcp::cli_trading_allowed(); });
```
For settings-write: the headless gate denies settings-write tools unless `cli_settings_write_allowed()`. Implement by checking the tool's category/name in the dispatch path (in `HeadlessRuntime::call_tool`, if the tool is a settings-write tool and `!cli_settings_write_allowed()`, return `ToolResult::fail("settings write disabled — enable in GUI Settings")`).

- [ ] **Step 4: Attach enforcement** — `TerminalMcpBridge.cpp`: when building `bridge.json` (the Phase-1 writer), include the `destructive_token` field ONLY when `mcp::cli_trading_allowed()` is true; re-write the file when the setting changes (hook the bridge to a settings-changed signal, or re-evaluate on each `start()`; for 2a, evaluating at `start()` + on an existing settings-change signal is sufficient). Settings-write tools are gated server-side in `McpProvider`/SettingsTools on `cli_settings_write_allowed()`.

- [ ] **Step 5: GUI toggles** — add two `QCheckBox`/toggle rows to the Settings screen (the General or a new "CLI access" section) bound to `cli.allow_settings_write` and `cli.allow_trading` via `SettingsRepository`. (Locate the settings screen: `grep -rl "class .*SettingsSection" src/screens/settings`.)

- [ ] **Step 6: Tests + smoke:**
```bash
cmake --build /tmp/ot-build-test --target tst_settings_gate && ctest --test-dir /tmp/ot-build-test -R tst_settings_gate --output-on-failure
# headless gate behavior:
/tmp/ot-build-ht/openterminalcli --headless mcp call place_order '{}' ; echo "exit=$?"   # denied (5) when off
/tmp/ot-build-ht/openterminalcli config set cli.allow_trading true
/tmp/ot-build-ht/openterminalcli --headless mcp call <a safe destructive-category tool> ; echo "exit=$?"  # now permitted by gate
/tmp/ot-build-ht/openterminalcli config set cli.allow_trading false
```
Expected: destructive denied (exit 5) when off; gate permits when on. (Use a destructive-category tool that fails safely without real side effects for the "permitted" check, or assert the gate no longer denies via logs.)

- [ ] **Step 7: Commit**
```bash
git add src/mcp/tools/SettingsGate.h src/mcp/tools/SettingsGate.cpp src/core/headless/HeadlessRuntime.cpp src/mcp/TerminalMcpBridge.cpp src/screens/settings/* CMakeLists.txt tests/tst_settings_gate.cpp tests/CMakeLists.txt
git commit -m "feat(cli): cli.allow_settings_write + cli.allow_trading gates (GUI toggles, server-enforced)"
```

---

## Task 7: Regression gate + headless e2e

**Files:** Create `tests/e2e_headless_smoke.sh`.

- [ ] **Step 1: e2e script** — `tests/e2e_headless_smoke.sh`: with NO GUI running, run `openterminalcli --headless` `status`/`version`/`mcp list`/`quote AAPL`/`hub topics`/`selftest tools`; assert each; assert a destructive call is denied with `cli.allow_trading=false`. (No app to launch — headless is self-contained.)

- [ ] **Step 2: Full regression:**
```bash
ctest --test-dir /tmp/ot-build-test --output-on-failure                 # all tst_* green
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds bridge-discovery paper portfolio-replication; do "$APP" --selftest-$t; echo "$t exit=$?"; done
bash tests/e2e_headless_smoke.sh
```
Expected: all unit tests pass; GUI selftests exit 0 (no regression from the lib extraction); headless e2e passes.

- [ ] **Step 3: Verify Phase-1 attach still works** (launch GUI, `openterminalcli status` attached, `mcp call get_quote` — same as Phase-1 e2e). If the env's GUI auth-modal/TCC blocks a full run, run what's reachable and note it.

- [ ] **Step 4: Commit**
```bash
git add tests/e2e_headless_smoke.sh
git commit -m "test(cli): headless e2e smoke + Phase-2a regression gate"
```

---

## Plan 2b (NOT detailed here — separate plan)
Widen headless coverage to the full read+write data-tool set: for each remaining data category, bring its backing service up in `HeadlessRuntime` (lazily per command), vet for `QCoreApplication`-safety, and wire `cli.allow_settings_write` to every settings-write tool. Each service = repeat of the Task-4/5 pattern + a per-service headless smoke. Build it after 2a is merged and the foundation is proven.

---

## Self-Review
**Spec coverage:** lib extraction → T3; DataHub decouple → T1; McpInit split → T2; HeadlessRuntime → T4; `--headless` → T5; two gates + GUI toggles + conditional destructive_token → T6; no-regression gate + headless e2e → T3.5/T7; capability table (default-on categories) → T2 (core set) + T6 (gates). The broad data-tool widening is explicitly deferred to 2b (noted).

**Known discovery points (bounded by tests, not placeholders):** (a) the exact GUI-coupled file list in T3 is verified by the linker (the gate: lib builds Core-only); (b) the no-network core tool for the T4 assertion and the minimal service bring-up are chosen against `grep` of real tool names + bounded by the smoke test; (c) the settings-screen file in T6 is located by grep. These are verification-bounded, not vague.

**Type consistency:** `set_owner_active_hook`/`owner_active_for_test` (T1), `register_core_tools`/`register_gui_tools` (T2), `openterminal_core` (T3), `HeadlessRuntime::{init,call_tool,shutdown}` + `InitResult` (T4), `GlobalOpts::headless` + `emit_result` overload (T5), `cli_trading_allowed`/`cli_settings_write_allowed` (T6) are used consistently across tasks.

**Placeholder scan:** no "TBD/handle errors/similar to" — code steps carry real code; the few discovery points name the exact grep + the test that bounds them.
