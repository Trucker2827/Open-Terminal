# Off-Thread Bridge Tool Dispatch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Dispatch bridge tool calls off the event-loop thread (single worker) so a slow/sync tool can't freeze the daemon; add `serve --stop` SIGKILL-after-grace.

**Architecture:** `TerminalMcpBridge::handle_post_tool` runs the internal tool call on a bridge-owned `QThreadPool` (max 1 thread) via the sync `McpProvider::call_tool` inside `QtConcurrent::run`, with the `CallFlagGuard` set on the worker; the existing `QFutureWatcher` marshals the result back to the main thread for the socket write. Serialized = no new concurrent service access.

**Tech Stack:** C++20, Qt6 (Core/Network/Concurrent), CMake+Ninja, QtTest. POSIX signals.

**Spec:** `openmarketterminal-qt/docs/design/2026-06-14-bridge-offthread-dispatch-design.md`
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` · test build `/tmp/ot-build-test` · **Branch:** create `feat/bridge-offthread-dispatch` off `main` (`5beca0a`) before Task 1.

---

## Verified facts (current `handle_post_tool`, internal branch)
```cpp
const bool destructive_ok = !destructive_token_.isEmpty() && destructive_hdr == destructive_token_;
QFuture<ToolResult> future;
{
    CallFlagGuard guard(destructive_ok);                 // set on the MAIN thread (the bug)
    if (server_id == INTERNAL_SERVER_ID) {
        future = McpProvider::instance().call_tool_async(tool_name, args);  // sync handler runs INLINE on main
    } else {
        const QString fn = server_id + "__" + McpProvider::encode_tool_name_for_wire(tool_name);
        future = McpService::instance().execute_openai_function_async(fn, args);  // already off-main
    }
}
auto* watcher = new QFutureWatcher<ToolResult>(this);
connect(watcher, &QFutureWatcher<ToolResult>::finished, this, [self, sock_guard, tool_name, call_id, watcher]() {
    ToolResult result = ...; self->write_json_response(sock_guard, 200, payload); ...  // MAIN thread (keep)
});
watcher->setFuture(future);
```
- `CallFlagGuard` is an anon-namespace RAII in `TerminalMcpBridge.cpp` that sets file-static `thread_local` flags `tls_call_in_progress`/`tls_destructive_allowed`, read by `TerminalMcpBridge::is_call_in_progress()`/`is_destructive_allowed()` (which the McpProvider auth-checker consults). Thread-locals → must be set on the SAME thread that runs the auth check (which is inside `call_tool`).
- `McpProvider::call_tool(name,args)` is the sync wrapper (`call_tool_async` + `.result()`), used off-main by `HeadlessRuntime::call_tool` today.
- `openterminal_core` links `Qt6::Concurrent`; `BridgeClient` (`src/cli/BridgeClient.{h,cpp}`) is the Phase-1 HTTP client (constructed from `BridgeInfo{endpoint,token,...}`).
- `serve_stop` (`src/cli/ServeCommand.cpp`) currently SIGTERMs and returns; `serve --stop` shells through `serve_stop`.

---

## File Structure
- Modify: `src/mcp/TerminalMcpBridge.h` (add `QThreadPool tool_pool_;`), `src/mcp/TerminalMcpBridge.cpp` (off-thread dispatch).
- Modify: `src/cli/ServeCommand.cpp` (`serve_stop` SIGKILL-after-grace).
- Create: `tests/tst_bridge_offthread.cpp` (off-main proof + guard-on-worker via real bridge + BridgeClient).
- Modify: `tests/CMakeLists.txt`; `tests/e2e_daemon_smoke.sh` (add the responsiveness assertion).

---

## Task 1: Off-thread dispatch in `handle_post_tool` (the fix) + off-main proof

**Files:** Modify `src/mcp/TerminalMcpBridge.h`, `src/mcp/TerminalMcpBridge.cpp`; Create `tests/tst_bridge_offthread.cpp`; Modify `tests/CMakeLists.txt`.

- [ ] **Step 1: Add the single-worker pool member** — `TerminalMcpBridge.h`, in the private members:
```cpp
    QThreadPool tool_pool_;   // serialized off-main tool dispatch (maxThreadCount=1)
```
Add `#include <QThreadPool>`. In the constructor (`TerminalMcpBridge::TerminalMcpBridge(QObject* parent)` in the .cpp) set `tool_pool_.setMaxThreadCount(1);`.

- [ ] **Step 2: Move the internal dispatch off-thread** — `TerminalMcpBridge.cpp`, in `handle_post_tool`, replace the `{ CallFlagGuard guard(...); if(internal) future = call_tool_async(...); else ... }` block with:
```cpp
    QFuture<ToolResult> future;
    if (server_id == INTERNAL_SERVER_ID) {
        // Run the (possibly blocking, sync) handler on a dedicated single worker
        // so the event loop stays responsive (SIGTERM/serve --stop work). The
        // CallFlagGuard MUST be set on this worker thread, because the auth
        // checker reads the thread-local flags during dispatch (which now runs
        // here, not on the main thread). Serialized (pool size 1) → no new
        // concurrent access to services.
        future = QtConcurrent::run(&tool_pool_, [tool_name, args, destructive_ok]() {
            CallFlagGuard guard(destructive_ok);
            return McpProvider::instance().call_tool(tool_name, args);
        });
    } else {
        CallFlagGuard guard(destructive_ok);   // external path is already non-blocking on main
        const QString fn = server_id + "__" + McpProvider::encode_tool_name_for_wire(tool_name);
        future = McpService::instance().execute_openai_function_async(fn, args);
    }
```
Add `#include <QtConcurrent>` (or `<QtConcurrent/QtConcurrentRun>`). Leave the `QFutureWatcher` block below unchanged (it already marshals to the main thread). NOTE: the external branch keeps the guard on the main thread (unchanged behavior — that path doesn't deadlock); only the internal branch moves to the worker.

- [ ] **Step 3: Write the off-main + result test** — `tests/tst_bridge_offthread.cpp`. Drives the REAL bridge via `BridgeClient` against a registered probe tool that records the thread it ran on:
```cpp
#include <QtTest>
#include <QThread>
#include <QCoreApplication>
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "cli/BridgeDiscoveryFile.h"
#include "cli/BridgeClient.h"
#include <atomic>
using namespace openmarketterminal;
class TstBridgeOffthread : public QObject {
    Q_OBJECT
    static inline std::atomic<Qt::HANDLE> probe_thread{nullptr};
private slots:
    void tool_runs_off_the_main_thread() {
        // Register a probe tool that records its running thread.
        mcp::ToolDef probe;                 // (use the real ToolDef fields — see McpTypes.h)
        probe.name = "tst_probe_thread";
        probe.category = "system";
        probe.handler = [](const QJsonObject&) -> mcp::ToolResult {
            probe_thread = QThread::currentThreadId();
            return mcp::ToolResult::ok("ok");
        };
        mcp::McpProvider::instance().register_tools({probe});

        auto& bridge = mcp::TerminalMcpBridge::instance();
        QVERIFY(bridge.start());
        cli::BridgeInfo info; info.endpoint = bridge.endpoint(); info.token = bridge.token();
        cli::BridgeClient client(info);
        auto r = client.call_tool("tst_probe_thread", {});
        QCOMPARE(r.status, cli::ClientStatus::Ok);
        QVERIFY(r.body.value("success").toBool());
        QVERIFY(probe_thread.load() != nullptr);
        QVERIFY2(probe_thread.load() != QThread::currentThreadId(),
                 "tool ran on the main/test thread — off-thread dispatch broken");
        bridge.stop();
    }
};
QTEST_MAIN(TstBridgeOffthread)
#include "tst_bridge_offthread.moc"
```
(Verify the real `ToolDef`/registration API in `src/mcp/McpTypes.h` + `McpProvider.h` — adjust field names; the load-bearing assertion is `probe_thread != main thread` + a successful round-trip. `QTEST_MAIN` provides the QCoreApplication the bridge + BridgeClient need.) Add the target to `tests/CMakeLists.txt` linking `openterminal_core Qt6::Core Qt6::Network Qt6::Test` and compiling `src/cli/BridgeClient.cpp` + `src/cli/BridgeDiscoveryFile.cpp`.

- [ ] **Step 4: Run — verify it FAILS before the Step-2 change** (revert Step 2 mentally / it would run on main) and PASSES after:
```bash
cmake --build /tmp/ot-build-test --target tst_bridge_offthread && ctest --test-dir /tmp/ot-build-test -R tst_bridge_offthread --output-on-failure
```
Expected after the fix: PASS (probe ran off the main thread, result returned). If you want to see RED first, temporarily change Step 2's internal branch back to inline `call_tool_async` and confirm the assertion fails (`probe_thread == main`), then restore.

- [ ] **Step 5: Guard-on-worker check** — add a second slot proving the `CallFlagGuard` thread-locals work on the worker: install an auth-checker mimicking AgentService (`return mcp::TerminalMcpBridge::is_destructive_allowed();`), register a destructive probe tool (`probe.is_destructive = true`), then call it via `BridgeClient` (a) WITHOUT the destructive header → denied (`success==false` / auth error), and (b) the round-trip still completes (no deadlock). (Sending the `X-MCP-Allow-Destructive` header from the test requires BridgeClient support or a raw socket; if BridgeClient can't set it, assert only the deny path — which still proves the guard is consulted on the worker. State which you did.) Restore the default auth-checker after.

- [ ] **Step 6: GUI links + a quick bridge selftest** (the bridge is in the GUI too):
```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal && /tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-bridge-discovery; echo "exit=$?"
```
Expected: links, exit 0.

- [ ] **Step 7: Commit**
```bash
git add src/mcp/TerminalMcpBridge.h src/mcp/TerminalMcpBridge.cpp tests/tst_bridge_offthread.cpp tests/CMakeLists.txt
git commit -m "fix(bridge): dispatch internal tool calls off the event-loop thread (single worker)"
```

---

## Task 2: `serve --stop` SIGKILL-after-grace

**Files:** Modify `src/cli/ServeCommand.cpp`; Test extend `tests/tst_serve_command.cpp`.

- [ ] **Step 1: Escalate to SIGKILL after a grace window** — in `serve_stop` (`ServeCommand.cpp`), replace the single `kill(pid, SIGTERM)` + return with:
```cpp
#ifndef _WIN32
    if (::kill(static_cast<pid_t>(info->pid), SIGTERM) != 0) {
        std::fprintf(stderr, "failed to signal daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    // Grace: wait up to ~5s for clean exit; escalate to SIGKILL if still alive.
    for (int i = 0; i < 50 && is_pid_alive(info->pid); ++i)
        QThread::msleep(100);
    if (is_pid_alive(info->pid)) {
        std::fprintf(stderr, "daemon pid %lld did not exit on SIGTERM; sending SIGKILL\n",
                     static_cast<long long>(info->pid));
        ::kill(static_cast<pid_t>(info->pid), SIGKILL);
        for (int i = 0; i < 20 && is_pid_alive(info->pid); ++i) QThread::msleep(100);
        // A SIGKILLed daemon can't run its aboutToQuit cleanup, so the stale
        // bridge.json may remain; remove it so the next attach/serve is clean.
        remove_bridge_file(profile_root_for(profile));
        std::printf("daemon pid %lld force-stopped (SIGKILL)\n", static_cast<long long>(info->pid));
        return 0;
    }
    std::printf("sent SIGTERM to daemon pid %lld\n", static_cast<long long>(info->pid));
    return 0;
#endif
```
Add `#include <QThread>`. (Keep the non-daemon-owner refusal + not-running checks above unchanged.)

- [ ] **Step 2: Test** — in `tests/tst_serve_command.cpp`, add a slot for the force path that doesn't depend on a real wedged process: factor the escalation into a small testable helper if practical, OR assert the existing refuse/not-running branches still return 3 (the SIGKILL escalation itself is covered by the e2e in Task 3, which wedges a real daemon). State which. Keep the existing 3 slots green.
```bash
cmake --build /tmp/ot-build-test --target tst_serve_command && ctest --test-dir /tmp/ot-build-test -R tst_serve_command --output-on-failure
```
Expected: PASS.

- [ ] **Step 3: Commit**
```bash
git add src/cli/ServeCommand.cpp tests/tst_serve_command.cpp
git commit -m "feat(cli): serve --stop SIGKILL-after-grace (always-stoppable daemon)"
```

---

## Task 3: Daemon-responsiveness e2e + full regression (the user's 6 acceptance gates)

**Files:** Modify `tests/e2e_daemon_smoke.sh`.

- [ ] **Step 1: Add the responsiveness assertion** to `tests/e2e_daemon_smoke.sh` — after the daemon is up, fire a tool that exercises the off-main path AND prove the daemon stays controllable. Concretely: kick off a `mcp call get_quote` in the BACKGROUND (it may be slow/cold), then immediately assert `serve --status` responds within ~3s and `serve --stop` stops the daemon within ~5s and removes bridge.json — i.e. the loop isn't wedged by the in-flight tool:
```bash
# --- responsiveness under an in-flight (possibly slow) tool ---
timeout 60 "$CLI" --profile "$PROF" mcp call get_quote '{"symbol":"AAPL"}' >/dev/null 2>&1 &
QPID=$!
sleep 1   # let the call be in flight on the worker
timeout 5 "$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" || fail "status unresponsive while a tool is in flight"
echo "PASS: --status responsive during in-flight tool"
timeout 8 "$CLI" --profile "$PROF" serve --stop | grep -qiE "SIGTERM|force-stopped" || fail "--stop unresponsive while a tool is in flight"
sleep 1; test -f "$ROOT/bridge.json" && fail "bridge.json remained after stop" || echo "PASS: --stop worked + bridge.json removed under load"
kill "$QPID" 2>/dev/null
```
(Keep the existing e2e assertions: kind=daemon, attach, second-serve-refused, read-only set_setting denied. Use the throwaway `--profile daemon-e2e-$$` and `timeout` watchdogs throughout.)

- [ ] **Step 2: Run the e2e:**
```bash
bash tests/e2e_daemon_smoke.sh
```
Expected: all PASS, including the new responsiveness assertion (the daemon stays controllable while a tool is in flight — the core fix proof). This is **user acceptance tests #1 + #2**.

- [ ] **Step 3: Full regression — the user's acceptance gates #3–#6 (paste output):**
```bash
ctest --test-dir /tmp/ot-build-test --output-on-failure                          # all tst_* incl offthread + serve
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds dock-layout universe-scan paper portfolio-replication bridge-discovery; do "$APP" --selftest-$t; echo "$t exit=$?"; done    # #5 GUI selftests
"$APP" --selftest-workflow-honesty; echo "exit=$?"
/tmp/ot-build-ht/openterminalcli --headless mcp list | head -c 80; echo          # #6 headless one-shot
# #3 read-only gate (daemon): set_setting denied — covered by the e2e's set_setting assertion
```
Expected: all unit tests pass; all GUI selftests exit 0 (#5); headless one-shot returns the catalog (#6).

- [ ] **Step 4: #3 read-only + #4 GUI attach (paste):**
- #3 is asserted in the e2e (set_setting via daemon → exit 5). Also confirm a destructive tool denied: `"$CLI" --profile <daemon> mcp call live_cancel_all_orders '{}'; echo exit=$?` → 5.
- #4 (best-effort) launch the GUI, `openterminalcli status` shows `kind=gui` attached + `mcp call get_quote` works (the off-thread change must not break the GUI's bridge/agent path). If env blocks GUI automation, run what's reachable + note honestly.

- [ ] **Step 5: Commit**
```bash
git add tests/e2e_daemon_smoke.sh
git commit -m "test(cli): daemon stays controllable under an in-flight tool (off-thread dispatch)"
```

---

## Self-Review
**Spec coverage:** off-thread single-worker dispatch + CallFlagGuard-on-worker → T1; external branch unchanged → T1 Step 2; serve --stop SIGKILL-after-grace → T2; the 6 acceptance tests → #1/#2 (T3 e2e responsiveness + SIGTERM cleanup), #3 (T3.4 destructive/set_setting denied), #4 (T3.4 GUI attach), #5 (T3.3 selftests), #6 (T3.3 headless one-shot); off-main proof + guard-on-worker → T1 Steps 3/5.

**Known discovery points (verification-bounded):** the exact `ToolDef` registration API for the probe (grep `McpTypes.h`/`McpProvider.h`); whether `BridgeClient` can set the `X-MCP-Allow-Destructive` header (if not, assert the deny path only) — both named with the grep + a stated fallback.

**Type consistency:** `tool_pool_`, `CallFlagGuard`, `McpProvider::call_tool`, `BridgeClient`/`BridgeInfo`/`ClientStatus`, `is_destructive_allowed`, `serve_stop`, `is_pid_alive`/`remove_bridge_file`/`profile_root_for` all match shipped names.

**Placeholder scan:** code steps complete; the probe-tool field names are the one spot the engineer reconciles against the real `ToolDef` (grep given).
