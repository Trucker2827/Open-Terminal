# openterminalcli Phase 2b (Widen Headless Coverage) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the full data-tool set work headless by bringing up the GUI's 11 data services in `HeadlessRuntime`, then verifying gating + per-category function.

**Architecture:** A shared `register_all_data_services()` helper (mirrors `register_all_migrations()`) registers the 11 data services with the DataHub; both the GUI and `HeadlessRuntime::init()` call it. Registration is cheap producer-wiring (no fetch); the one-shot-safety gate ensures no service connects a feed / spawns a process at registration. The capability gates from 2a are then verified across the widened set.

**Tech Stack:** C++20, Qt6 (Core/Network/Sql/Widgets), CMake+Ninja, QtTest (`-DOPENMARKETTERMINAL_BUILD_TESTS=ON`).

**Spec:** `openmarketterminal-qt/docs/design/2026-06-14-openterminalcli-phase2b-widen-design.md`
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` · test build `/tmp/ot-build-test` · **Branch:** create `feat/openterminalcli-phase2b` off `main` (`d7e574c`) before Task 1.

---

## Verified facts (the registration map)
`main.cpp` registers data producers in two phases:
- **Eager (345–361):** `MarketDataService` (345), `NewsService` (353), `EconomicsService` (354), `MacroCalendarService` (355), `geo::GeopoliticsService` (357), `maritime::MaritimeService` (358), `RelationshipMapService` (360), `ma::MAAnalyticsService` (361). (Also `DataStreamManager` 356 + `maritime::PortsCatalog` 359 — NOT in the 11; leave them.)
- **Deferred** in `QTimer::singleShot(0)` (post-paint, 421–466): `DBnomicsService` (460), `GovDataService` (461), `AgentService` (463). (Also Polymarket/ExchangeSession/AlgoEngine — NOT in the 11; leave them.)

**The 11 data services** (full namespaces): `services::MarketDataService`, `services::NewsService`, `services::EconomicsService`, `services::MacroCalendarService`, `services::geo::GeopoliticsService`, `services::maritime::MaritimeService`, `services::RelationshipMapService`, `services::ma::MAAnalyticsService`, `services::DBnomicsService`, `services::GovDataService`, `services::AgentService`.

`HeadlessRuntime::init()` already calls `MarketDataService::instance().ensure_registered_with_hub()`. `register_all_migrations()` (the no-drift precedent) lives in `src/storage/sqlite/migrations/RegisterAllMigrations.cpp`.

---

## File Structure
- Create: `src/services/DataServices.h` / `.cpp` (the shared `register_all_data_services()` helper) — added to `SERVICE_SOURCES` → `openterminal_core`.
- Modify: `src/core/headless/HeadlessRuntime.cpp` (call the helper instead of the lone MarketDataService line), `src/app/main.cpp` (call the helper; keep non-11 producers as-is), possibly `MaritimeService`/`AgentService` (only if registration is NOT cheap — to add a headless-safe guard).
- Create: `tests/tst_data_services.cpp` (drift guard + all-11-registered + fast-init), per-category smoke additions.

---

## Task 1: Shared `register_all_data_services()` + bring up all 11 in headless (one-shot-safe)

**Files:** Create `src/services/DataServices.{h,cpp}`; Modify `src/core/headless/HeadlessRuntime.cpp`, `src/app/main.cpp`, `CMakeLists.txt`; Test `tests/tst_data_services.cpp`.

- [ ] **Step 1: Verify registration is cheap BEFORE wiring it eager (the safety check).** For the 3 GUI-deferred ones especially, read what `ensure_registered_with_hub()` does:
```bash
cd ~/src/Open-Terminal/openmarketterminal-qt
for s in AgentService MaritimeService DBnomicsService GovDataService; do
  echo "=== $s ==="; f=$(grep -rl "void .*$s::ensure_registered_with_hub" src/services 2>/dev/null | head -1); \
  sed -n "/ensure_registered_with_hub/,/^}/p" "$f" 2>/dev/null | head -40; done
```
Expected: each only wires DataHub producers/subscriptions (no `QProcess`/`start(`, no `connectToHost`/websocket `open(`, no blocking wait). RECORD what each does. If `AgentService` or `MaritimeService` registration spawns a process or opens a socket synchronously, note it — Step 4 guards it.

- [ ] **Step 2: Write the failing test** — `tests/tst_data_services.cpp`:
```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "services/DataServices.h"
#include "datahub/DataHub.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
using namespace openmarketterminal;
class TstDataServices : public QObject {
    Q_OBJECT
private slots:
    void registers_all_eleven_fast() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        ProfileManager::instance().set_active("default");
        QDir().mkpath(AppPaths::data());
        register_all_migrations();
        Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
        QElapsedTimer t; t.start();
        services::register_all_data_services();          // must return fast, no hang
        QVERIFY2(t.elapsed() < 5000, "register_all_data_services took too long (blocking/connecting?)");
        // The hub now has producers registered (topic count grew); a representative
        // data topic for a registered producer is known to the hub.
        QVERIFY(datahub::DataHub::instance().topic_count() > 0); // see note: use the real introspection API
    }
};
QTEST_MAIN(TstDataServices)
#include "tst_data_services.moc"
```
NOTE: replace `topic_count()` with the real DataHub introspection call (`grep -n "int .*topic\|registered_topics\|known_topics\|producer" src/datahub/DataHub.h` — use what exists; if none, assert each service's `is_registered()`/equivalent, or simply that the call returns without hang and a subsequent `MarketDataService` peek path works). The load-bearing assertion is **fast, non-blocking completion**. Add the target to `tests/CMakeLists.txt` linking `openterminal_core Qt6::Core Qt6::Test`.

- [ ] **Step 3: Run — FAIL** (`register_all_data_services` undefined):
```bash
cmake --build /tmp/ot-build-test --target tst_data_services
```

- [ ] **Step 4: Implement the helper** — `src/services/DataServices.h`:
```cpp
#pragma once
namespace openmarketterminal::services {
/// Register the 11 data-producing services with the DataHub. Cheap (producer
/// wiring only — no fetch/connect/spawn). Called by the GUI and HeadlessRuntime.
void register_all_data_services();
}
```
`src/services/DataServices.cpp`:
```cpp
#include "services/DataServices.h"
#include "services/markets/MarketDataService.h"
#include "services/news/NewsService.h"
#include "services/economics/EconomicsService.h"
#include "services/economics/MacroCalendarService.h"
#include "services/geopolitics/GeopoliticsService.h"
#include "services/maritime/MaritimeService.h"
#include "services/relationship_map/RelationshipMapService.h"
#include "services/ma_analytics/MAAnalyticsService.h"
#include "services/dbnomics/DBnomicsService.h"
#include "services/gov_data/GovDataService.h"
#include "services/agents/AgentService.h"
namespace openmarketterminal::services {
void register_all_data_services() {
    MarketDataService::instance().ensure_registered_with_hub();
    NewsService::instance().ensure_registered_with_hub();
    EconomicsService::instance().ensure_registered_with_hub();
    MacroCalendarService::instance().ensure_registered_with_hub();
    geo::GeopoliticsService::instance().ensure_registered_with_hub();
    maritime::MaritimeService::instance().ensure_registered_with_hub();
    RelationshipMapService::instance().ensure_registered_with_hub();
    ma::MAAnalyticsService::instance().ensure_registered_with_hub();
    DBnomicsService::instance().ensure_registered_with_hub();
    GovDataService::instance().ensure_registered_with_hub();
    AgentService::instance().ensure_registered_with_hub();
}
}
```
(Adjust each include path/namespace to the real header — verify with `grep -rl "class MarketDataService" src/services` etc. The namespaces are from the main.cpp call sites above.) Add `src/services/DataServices.cpp` to `SERVICE_SOURCES` in `CMakeLists.txt`.

**Guard (only if Step 1 found heavy work):** if `AgentService`/`MaritimeService` registration spawns/connects synchronously, add a headless-safe path so the producer is wired without starting the feed/subprocess (e.g. a parameter or a separate lightweight `register_producer_only()` the helper calls; the feed/subprocess starts on explicit subscription, which a one-shot CLI never triggers). Keep the GUI behavior unchanged.

- [ ] **Step 5: Wire both callers.**
  - `HeadlessRuntime.cpp`: replace the single `MarketDataService::instance().ensure_registered_with_hub();` line with `openmarketterminal::services::register_all_data_services();` (add `#include "services/DataServices.h"`).
  - `main.cpp`: replace the 8 eager individual calls (lines ~345–361, the 8 that are in the 11 — keep `DataStreamManager` + `PortsCatalog`) with a single `openmarketterminal::services::register_all_data_services();`, and REMOVE the 3 deferred ones (DBnomics/GovData/AgentService) from the `singleShot` batch (now covered by the helper). Keep Polymarket/ExchangeSession/AlgoEngine in the deferred batch. (If Step-1 found AgentService registration heavy and you do NOT want it eager in the GUI, instead: GUI keeps AgentService deferred and the helper is headless-only — then add the drift-guard test from Step 2 to catch list divergence. State which path you took.)

- [ ] **Step 6: Run test — PASS:**
```bash
cmake --build /tmp/ot-build-test --target tst_data_services && ctest --test-dir /tmp/ot-build-test -R tst_data_services --output-on-failure
```

- [ ] **Step 7: One-shot-safety + GUI no-regression gates (paste output):**
```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli
# headless init must be fast + not spawn python / open AIS socket:
time /tmp/ot-build-ht/openterminalcli --headless version; echo "exit=$?"
# confirm no lingering python/websocket from a one-shot:
/tmp/ot-build-ht/openterminalcli --headless version >/dev/null 2>&1; pgrep -f "python3|aisstream" | wc -l   # expect 0 attributable to the CLI
# GUI no-regression:
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds paper bridge-discovery; do "$APP" --selftest-$t; echo "$t exit=$?"; done
```
Expected: `--headless version` returns in ~1s, exit 0; no CLI-spawned python/AIS; all selftests exit 0.

- [ ] **Step 8: Commit**
```bash
git add src/services/DataServices.h src/services/DataServices.cpp src/core/headless/HeadlessRuntime.cpp src/app/main.cpp CMakeLists.txt tests/tst_data_services.cpp tests/CMakeLists.txt
git commit -m "feat(headless): bring up all 11 data services via shared register_all_data_services()"
```

---

## Task 2: Verify capability gating across the widened set

**Files:** Modify `tests/tst_settings_gate.cpp` (add coverage); possibly fix a tool definition if a settings-write tool isn't flagged.

- [ ] **Step 1: Enumerate the settings-write surface + confirm classification.**
```bash
cd ~/src/Open-Terminal/openmarketterminal-qt
grep -n 't.name =\|t.category = "settings"\|is_destructive' src/mcp/tools/SettingsTools.cpp
```
Confirm: every `category=="settings"` tool that MUTATES config is `is_destructive=true` (so `is_settings_write_tool` + the `cli.allow_settings_write` gate covers it); every read settings tool is NOT destructive (always allowed). RECORD the full list. If a settings-write tool is missing `is_destructive=true`, fix its definition (so the gate covers it) — that's the only code change this task may need.

- [ ] **Step 2: Confirm the headless catalog stays GUI-free + gated.**
```bash
/tmp/ot-build-ht/openterminalcli --headless mcp list > /tmp/cat.json
python3 - <<'PY'
import json; d=json.load(open('/tmp/cat.json')); names={t['name'] for t in d['tools']}
print('count', len(names))
for gui in ('navigate_to_tab','open_dashboard','export_to_excel'):  # representative GUI tools
    print(gui, 'present?' , gui in names)   # all should be False
for data in ('get_quote',):
    print(data, 'present?', data in names)  # True
PY
```
Expected: GUI tools absent, data tools present.

- [ ] **Step 3: Add gate-coverage assertions** to `tests/tst_settings_gate.cpp`: for a settings-write tool (`set_setting`), denied with `cli.allow_settings_write=false`, allowed when true (already covered — confirm still green); for a representative destructive data-category tool, denied with `cli.allow_trading=false`; the `>= Verified` floor denies an ExplicitConfirm tool regardless (already covered by the 2b-hardening test — confirm). Add only the assertions not already present.

- [ ] **Step 4: Run:**
```bash
cmake --build /tmp/ot-build-test --target tst_settings_gate && ctest --test-dir /tmp/ot-build-test -R tst_settings_gate --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add tests/tst_settings_gate.cpp $( [ -n "$SETTINGS_TOOL_FIX" ] && echo src/mcp/tools/SettingsTools.cpp )
git commit -m "test(headless): verify capability gating holds across the widened tool set"
```

---

## Task 3: Per-category headless smoke + full regression

**Files:** Create `tests/e2e_headless_categories.sh`.

- [ ] **Step 1: Write the per-category smoke** — `tests/e2e_headless_categories.sh`. For each enabled data category, call a representative tool headless and assert it RETURNS (exit 0 with data OR exit 5 on a clean network/no-data error) WITHOUT hang/crash, using a `timeout` watchdog. Use real tool names (verify each with `grep 't.name =' src/mcp/tools/<X>Tools.cpp`):
```bash
#!/bin/bash
CLI=/tmp/ot-build-ht/openterminalcli
fail(){ echo "FAIL: $1"; exit 1; }
run(){ # name, tool, args
  timeout 30 "$CLI" --headless mcp call "$2" "$3" >/dev/null 2>&1; local rc=$?
  [ $rc -eq 124 ] && fail "$1 HUNG"; [ $rc -ge 134 ] && fail "$1 CRASHED (rc=$rc)"
  echo "PASS: $1 returned (rc=$rc)"; }
run markets       get_quote            '{"symbol":"AAPL"}'
run news          get_news             '{"query":"markets"}'        # use the real news tool name
run macro         get_economic_series  '{}'                         # real FRED/economics tool
run geopolitics   get_gdelt_events     '{}'                         # real geo tool
run gov-data      <real gov tool>      '{}'
run m&a           <real ma tool>       '{}'
run relationship  <real relmap tool>   '{}'
run edgar         <real edgar tool>    '{"query":"AAPL"}'
run datahub       datahub_list_topics  '{}'
run portfolio     get_portfolio        '{}'                          # real portfolio-read tool
echo "PASS: all categories returned without hang/crash"
```
Replace each `<real …>` and the guessed names with the actual tool names from the grep. `chmod +x`.

- [ ] **Step 2: Run it:**
```bash
bash tests/e2e_headless_categories.sh
```
Expected: every category prints PASS (rc 0 or 5; no 124/13x). Note any category that legitimately has no headless-safe representative tool.

- [ ] **Step 3: Full regression gate (paste output):**
```bash
ctest --test-dir /tmp/ot-build-test --output-on-failure                       # all tst_* green
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli    # both link
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds dock-layout universe-scan paper portfolio-replication bridge-discovery; do "$APP" --selftest-$t; echo "$t exit=$?"; done
"$APP" --selftest-workflow-honesty; echo "exit=$?"
```
Expected: all unit tests pass; all GUI selftests exit 0 (no regression from the shared-helper extraction).

- [ ] **Step 4: Phase-1 attach still works (best-effort)** — launch GUI, `openterminalcli status` attached, `openterminalcli mcp call get_quote '{"symbol":"AAPL"}'`. If env blocks GUI launch automation, run what's reachable and note it.

- [ ] **Step 5: Commit**
```bash
git add tests/e2e_headless_categories.sh
git commit -m "test(headless): per-category smoke + Phase-2b regression gate"
```

---

## Self-Review
**Spec coverage:** §1 shared helper + all 11 → T1; §2 one-shot safety (AgentService/Maritime) → T1 Step 1/4/7; §3 capability gating verify → T2; §4 settings-write covers all → T2 Step 1/3; §5 per-category smoke → T3; testing/no-regression → T1 Step 7 + T3 Step 3.

**Known discovery points (bounded by gates, not placeholders):** (a) whether AgentService/Maritime registration is heavy → resolved by T1 Step 1 inspection + the fast-init gate, with a guard fallback; (b) the real DataHub introspection API for the test assertion → grep-named in T1 Step 2 (load-bearing assertion is fast non-blocking completion); (c) the real per-category tool names → grep-named in T3 Step 1. Each is verification-bounded.

**Type consistency:** `register_all_data_services()` (namespace `openmarketterminal::services`) used identically in T1's helper, HeadlessRuntime, main.cpp, and the test. `register_all_migrations()` referenced as the precedent. `is_settings_write_tool` / `cli.allow_settings_write` / `cli.allow_trading` match the shipped 2a names.

**Placeholder scan:** no TBD/"handle errors"; the `<real … tool>` markers in T3 are explicitly grep-resolved with the exact command given (not vague) — the engineer fills the verified names.
