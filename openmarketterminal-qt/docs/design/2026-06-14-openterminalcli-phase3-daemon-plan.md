# openterminalcli Phase 3 — `serve` Daemon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `openterminalcli serve` — a long-lived headless process that hosts the DataHub + 10 data services + the `TerminalMcpBridge` (no GUI), so the existing attach commands (`mcp`/`hub`/`quote`/`status`) work against it unchanged.

**Architecture:** `serve` reuses `HeadlessRuntime::init()` (DB+migrations+SecureStorage+DataHub+services+core tools), installs a **read-only** auth-checker, explicitly starts the bridge (tagging `bridge.json` `kind:"daemon"`), then runs `QCoreApplication::exec()`. A single owner per profile (refuse if a live `bridge.json` exists). SIGTERM/SIGINT → clean shutdown (bridge.stop() removes `bridge.json`). `serve --status`/`--stop` manage it.

**Tech Stack:** C++20, Qt6 (Core/Network/Sql), CMake+Ninja, QtTest. POSIX signals (daemon is Linux/macOS; Windows daemon = follow-up).

**Spec:** `openmarketterminal-qt/docs/design/2026-06-14-openterminalcli-phase3-daemon-design.md`
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · GUI build `/tmp/ot-build-ht` · test build `/tmp/ot-build-test` · **Branch:** create `feat/openterminalcli-phase3-daemon` off `main` (`c7b63cb`) before Task 1.

**MVP scope / deferred:** Implement `serve`/`--status`/`--stop`, single-owner, clean shutdown, daemon-mode bring-up, transparent attach. **Read-only over the bridge** (deny destructive + settings-write). DEFER (spec open questions): streaming `hub watch`, daemonize/detach, and destructive/write-over-daemon (needs the revocable-token + live-recheck design). Windows `serve` (signals are POSIX).

---

## Verified facts
- `src/cli/main.cpp`: `QCoreApplication app; … return dispatch(args);`. `QCoreApplication::exec()` is **static** — a `serve` handler can enter the loop without the app pointer.
- `src/cli/CommandDispatch.{h,cpp}`: `GlobalOpts{json,headless,profile}`, `parse_global_opts`, `dispatch(args)` → exit code. Commands are dispatched by `group` (version/status/mcp/hub/quote); attach uses `BridgeClient`, headless uses `HeadlessRuntime`.
- `HeadlessRuntime::init(profile)` (core lib) does: `register_all_migrations` → DB → SecureStorage → cache → DataHub default hook → `register_all_data_services()` (10 services, 2b) → `McpProvider::set_auth_checker(...)` (denies `>=Verified`; settings-write→`cli_settings_write_allowed`; destructive→`cli_trading_allowed`) → `register_core_tools()`. Returns `InitResult{ok,error}`. Requires a QCoreApplication to exist.
- `src/cli/BridgeDiscoveryFile.h`: `struct BridgeInfo{endpoint,token,pid,started_at}` + `write_bridge_file`/`read_bridge_file`/`remove_bridge_file`/`profile_root_for`/`is_pid_alive`. Used by both the bridge (writer) and the CLI (reader).
- `src/mcp/TerminalMcpBridge.{h,cpp}`: `start()` binds 127.0.0.1:ephemeral + mints token + writes `bridge.json` (via `cli::write_bridge_file`); `stop()` removes it + closes. `endpoint()`/`token()`/`is_active()`. The AgentService ctor only auto-starts the bridge under a `QApplication` (gui_mode) — under the daemon's `QCoreApplication` it does NOT, so the daemon starts it explicitly with no conflict.
- `mcp::McpProvider::set_auth_checker(std::function<bool(const QString& tool, const QJsonObject& args, mcp::AuthLevel required, bool is_destructive)>)`; `mcp::is_settings_write_tool(name)`; `mcp::AuthLevel` ordered (None=0…Verified=2…).

---

## File Structure
- Modify: `src/cli/BridgeDiscoveryFile.{h,cpp}` (add `kind` to BridgeInfo), `src/mcp/TerminalMcpBridge.{h,cpp}` (owner-kind setter; write `kind`).
- Create: `src/cli/ServeCommand.{h,cpp}` (the daemon: single-owner check, init, read-only checker, bridge start, signal handling, exec, shutdown; + status/stop helpers).
- Modify: `src/cli/CommandDispatch.{h,cpp}` (route `serve`), `CMakeLists.txt` (ServeCommand.cpp → openterminalcli sources).
- Create/Modify tests: `tests/tst_serve_command.cpp` (single-owner + status/stop logic), `tests/e2e_daemon_smoke.sh` (full lifecycle).

---

## Task 1: `kind` field in bridge.json (distinguish daemon vs GUI)

**Files:** Modify `src/cli/BridgeDiscoveryFile.{h,cpp}`, `src/mcp/TerminalMcpBridge.{h,cpp}`; Test extend `tests/tst_bridge_discovery_file.cpp`.

- [ ] **Step 1: Add `kind` to `BridgeInfo`** (`BridgeDiscoveryFile.h`): add `QString kind;  // "gui" | "daemon"` to the struct (after `started_at`).

- [ ] **Step 2: Write/read the field** (`BridgeDiscoveryFile.cpp`): in `write_bridge_file`, add `o["kind"] = info.kind.isEmpty() ? QStringLiteral("gui") : info.kind;`. In `read_bridge_file`, set `info.kind = o.value("kind").toString("gui");` (default "gui" for old files).

- [ ] **Step 3: Failing test** — in `tests/tst_bridge_discovery_file.cpp`, extend the round-trip test to set `in.kind = "daemon"` and assert `out->kind == "daemon"`; add a slot asserting a file written WITHOUT `kind` reads back as `"gui"` (back-compat). Run → FAIL (no `kind` member yet) → after Steps 1-2 → PASS:
```bash
cmake --build /tmp/ot-build-test --target tst_bridge_discovery_file && ctest --test-dir /tmp/ot-build-test -R tst_bridge_discovery_file --output-on-failure
```

- [ ] **Step 4: Bridge owner-kind setter** — `TerminalMcpBridge.h`: add `void set_owner_kind(const QString& kind);` + a member `QString owner_kind_ = "gui";`. `.cpp`: implement the setter; in the `bridge.json`-writing path (the `cli::BridgeInfo` it constructs in `start()`/`write_discovery_file`), set `info.kind = owner_kind_;`. (Locate the existing `cli::write_bridge_file(...)`/`BridgeInfo{...}` construction in TerminalMcpBridge.cpp and add the field.)

- [ ] **Step 5: GUI still tags itself "gui"** — default `owner_kind_ = "gui"` means the GUI (which never calls the setter) writes `kind:"gui"` automatically. No main.cpp change needed. Build the GUI + run `--selftest-bridge-discovery` to confirm the bridge still writes a valid file:
```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal && /tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-bridge-discovery; echo "exit=$?"
```
Expected: exit 0.

- [ ] **Step 6: Commit**
```bash
git add src/cli/BridgeDiscoveryFile.h src/cli/BridgeDiscoveryFile.cpp src/mcp/TerminalMcpBridge.h src/mcp/TerminalMcpBridge.cpp tests/tst_bridge_discovery_file.cpp
git commit -m "feat(bridge): tag bridge.json with kind (gui|daemon) for serve"
```

---

## Task 2: `ServeCommand` — the daemon (single-owner, init, read-only, bridge, signals, exec)

**Files:** Create `src/cli/ServeCommand.{h,cpp}`; Modify `src/cli/CommandDispatch.cpp`, `CMakeLists.txt`.

- [ ] **Step 1: Header** — `src/cli/ServeCommand.h`:
```cpp
#pragma once
#include <QString>
namespace openmarketterminal::cli {
// Run the daemon for `profile`. Blocks in the event loop until SIGTERM/SIGINT
// or a fatal init error. Returns a process exit code (0 clean, 3 already-owned,
// 7 init failure). status()/stop() are the management subcommands.
int serve_run(const QString& profile);
int serve_status(const QString& profile, bool json);
int serve_stop(const QString& profile);
}
```

- [ ] **Step 2: Implement `serve_run`** — `src/cli/ServeCommand.cpp`:
```cpp
#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include <QCoreApplication>
#include <QSocketNotifier>
#include <cstdio>
#include <csignal>
#include <optional>
#ifndef _WIN32
#  include <unistd.h>
#endif

namespace openmarketterminal::cli {
namespace {
#ifndef _WIN32
int g_sigfd[2] = {-1, -1};                       // self-pipe: handler writes, notifier reads
void on_signal(int) { char c = 1; ssize_t n = ::write(g_sigfd[1], &c, 1); (void)n; }
#endif
} // namespace

int serve_run(const QString& profile) {
    const QString root = profile_root_for(profile);
    // Single-owner: refuse if a live instance (GUI or daemon) already owns it.
    if (auto info = read_bridge_file(root); info && is_pid_alive(info->pid)) {
        std::fprintf(stderr, "An instance already owns profile '%s' (%s, pid %lld) at %s\n",
                     qUtf8Printable(profile), qUtf8Printable(info->kind),
                     static_cast<long long>(info->pid), qUtf8Printable(info->endpoint));
        return 3;
    }

    headless::HeadlessRuntime rt;
    if (auto r = rt.init(profile); !r.ok) {
        std::fprintf(stderr, "daemon init failed: %s\n", qUtf8Printable(r.error));
        return 7;
    }

    // Read-only over the bridge: deny destructive AND settings-write regardless
    // of toggles (writes/destructive over a long-lived daemon need the revocable
    // -token design — deferred). Reads + the >=Verified floor unchanged.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject&, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (is_destructive) return false;            // daemon MVP: no writes/destructive
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });

    auto& bridge = mcp::TerminalMcpBridge::instance();
    bridge.set_owner_kind("daemon");
    if (!bridge.start()) {                                 // binds 127.0.0.1 + writes bridge.json(kind=daemon)
        std::fprintf(stderr, "daemon: failed to start the bridge\n");
        return 7;
    }
    std::fprintf(stderr, "openterminalcli serve: %s (profile '%s', pid %lld). Ctrl-C / SIGTERM to stop.\n",
                 qUtf8Printable(bridge.endpoint()), qUtf8Printable(profile),
                 static_cast<long long>(QCoreApplication::applicationPid()));

#ifndef _WIN32
    // Clean shutdown on SIGTERM/SIGINT via the self-pipe trick (async-signal-safe).
    if (::pipe(g_sigfd) == 0) {
        auto* sn = new QSocketNotifier(g_sigfd[0], QSocketNotifier::Read, qApp);
        QObject::connect(sn, &QSocketNotifier::activated, qApp, []() { QCoreApplication::quit(); });
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
    }
#endif

    const int rc = QCoreApplication::exec();              // feeds/subscriptions live here
    bridge.stop();                                        // removes bridge.json
    return rc;
}
} // namespace openmarketterminal::cli
```
(Verify `HeadlessRuntime::init` does NOT itself start the bridge — it doesn't; the daemon starts it. Verify the auth-checker lambda signature matches `McpProvider::set_auth_checker` exactly via `grep -n "using AuthChecker\|set_auth_checker" src/mcp/McpProvider.h`.)

- [ ] **Step 3: Route `serve` in dispatch** — `CommandDispatch.cpp`, add near the top of the command handling (serve takes only `--profile`; ignore `--json` for `serve_run`):
```cpp
    if (group == "serve") {
        const QString sub = args.isEmpty() ? QString() : args.first();
        if (sub == "--status") return serve_status(opts.profile, opts.json);
        if (sub == "--stop")   return serve_stop(opts.profile);
        if (!args.isEmpty()) { std::fprintf(stderr, "usage: serve [--status|--stop]\n"); return 2; }
        return serve_run(opts.profile);   // blocks in the event loop
    }
```
Add `#include "cli/ServeCommand.h"`.

- [ ] **Step 4: CMake** — add `src/cli/ServeCommand.cpp` to the `openterminalcli` target sources in `CMakeLists.txt`.

- [ ] **Step 5: Build + a manual smoke (no GUI running):**
```bash
cmake --build /tmp/ot-build-ht --target openterminalcli
rm -f "$HOME/Library/Application Support/org.openterminal.OpenTerminal/bridge.json"
/tmp/ot-build-ht/openterminalcli serve >/tmp/serve.log 2>&1 &
SERVE_PID=$!; sleep 6
cat /tmp/serve.log
/tmp/ot-build-ht/openterminalcli status                                   # attached endpoint, pid
/tmp/ot-build-ht/openterminalcli --json mcp call get_quote '{"symbol":"AAPL"}'; echo "exit=$?"   # via daemon
/tmp/ot-build-ht/openterminalcli mcp call set_setting '{"key":"x","value":"1"}'; echo "exit=$?"   # DENIED (read-only daemon): exit 5
kill -TERM $SERVE_PID; sleep 2
test -f "$HOME/Library/Application Support/org.openterminal.OpenTerminal/bridge.json" && echo "BUG: bridge.json remained" || echo "clean: bridge.json removed on SIGTERM"
```
Expected: serve logs an endpoint; `status` shows attached; `get_quote` returns via the daemon (exit 0 or 5 on no-data, not a hang); `set_setting` denied (exit 5, read-only); SIGTERM removes `bridge.json`.

- [ ] **Step 6: Commit**
```bash
git add src/cli/ServeCommand.h src/cli/ServeCommand.cpp src/cli/CommandDispatch.cpp CMakeLists.txt
git commit -m "feat(cli): serve daemon (single-owner, read-only bridge, clean SIGTERM shutdown)"
```

---

## Task 3: `serve --status` and `serve --stop`

**Files:** Modify `src/cli/ServeCommand.cpp`; Test `tests/tst_serve_command.cpp`.

- [ ] **Step 1: Implement status/stop** — append to `ServeCommand.cpp`:
```cpp
#include <QJsonObject>
#include <QJsonDocument>
namespace openmarketterminal::cli {

int serve_status(const QString& profile, bool json) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool live = info && is_pid_alive(info->pid);
    if (json) {
        QJsonObject o{{"running", live}};
        if (live) { o["endpoint"]=info->endpoint; o["pid"]=info->pid; o["kind"]=info->kind; }
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (live) {
        std::printf("running  kind=%s  endpoint=%s  pid=%lld\n",
                    qUtf8Printable(info->kind), qUtf8Printable(info->endpoint),
                    static_cast<long long>(info->pid));
    } else {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
    }
    return live ? 0 : 3;
}

int serve_stop(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    if (!info || !is_pid_alive(info->pid)) {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    if (info->kind != "daemon") {
        std::fprintf(stderr, "owner is a %s, not a daemon — refusing to stop it (quit it directly)\n",
                     qUtf8Printable(info->kind));
        return 3;
    }
#ifndef _WIN32
    if (::kill(static_cast<pid_t>(info->pid), SIGTERM) != 0) {
        std::fprintf(stderr, "failed to signal daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
#endif
    std::printf("sent SIGTERM to daemon pid %lld\n", static_cast<long long>(info->pid));
    return 0;
}
} // namespace openmarketterminal::cli
```
(Add `#include <csignal>`/`<sys/types.h>` for `kill` if not already present.)

- [ ] **Step 2: Unit test** — `tests/tst_serve_command.cpp` (links `openterminal_core Qt6::Core Qt6::Test`): with a `QTemporaryDir` HOME, write a `bridge.json` via `write_bridge_file` with `kind="gui"` + a LIVE pid (`QCoreApplication::applicationPid()`) → `serve_stop` returns 3 ("refusing — gui"); with `kind="daemon"` + a DEAD pid (e.g. 999999999) → `serve_status` returns 3 (not running); with `kind="daemon"` + live self-pid, `serve_status` returns 0. (Don't actually SIGTERM self in the test — assert the refuse/not-running branches; the live-daemon kill is covered by the e2e in Task 4.) Add the target to `tests/CMakeLists.txt`.

- [ ] **Step 3: Run:**
```bash
cmake --build /tmp/ot-build-test --target tst_serve_command && ctest --test-dir /tmp/ot-build-test -R tst_serve_command --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**
```bash
git add src/cli/ServeCommand.cpp tests/tst_serve_command.cpp tests/CMakeLists.txt
git commit -m "feat(cli): serve --status / --stop (daemon-kind aware)"
```

---

## Task 4: End-to-end daemon lifecycle + regression

**Files:** Create `tests/e2e_daemon_smoke.sh`.

- [ ] **Step 1: e2e script** — `tests/e2e_daemon_smoke.sh` (chmod +x). With NO GUI running, on a throwaway `--profile daemon-e2e-$$`:
```bash
#!/bin/bash
CLI=/tmp/ot-build-ht/openterminalcli
PROF="daemon-e2e-$$"
ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal/profiles/$PROF"
fail(){ echo "FAIL: $1"; [ -n "$SPID" ] && kill -TERM "$SPID" 2>/dev/null; exit 1; }
"$CLI" --profile "$PROF" serve >/tmp/daemon-e2e.log 2>&1 & SPID=$!
sleep 6
"$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" || fail "status not daemon"
"$CLI" --profile "$PROF" status | grep -qi "attached\|endpoint" || fail "attach status failed"
timeout 30 "$CLI" --profile "$PROF" mcp call get_quote '{"symbol":"AAPL"}' >/dev/null 2>&1
rc=$?; [ $rc -eq 124 ] && fail "get_quote hung via daemon"; [ $rc -ge 134 ] && fail "crash"
echo "PASS: get_quote via daemon (rc=$rc)"
# second serve refused
timeout 10 "$CLI" --profile "$PROF" serve >/tmp/daemon-e2e2.log 2>&1; [ $? -eq 3 ] || fail "second serve not refused (exit !=3)"
echo "PASS: second serve refused"
# read-only: settings write denied
"$CLI" --profile "$PROF" mcp call set_setting '{"key":"x","value":"1"}'; [ $? -eq 5 ] && echo "PASS: settings-write denied" || fail "settings-write not denied"
# stop via --stop
"$CLI" --profile "$PROF" serve --stop | grep -q "SIGTERM" || fail "--stop failed"; sleep 2
test -f "$ROOT/bridge.json" && fail "bridge.json remained after stop" || echo "PASS: bridge.json removed after --stop"
rm -rf "$ROOT"
echo "PASS: daemon e2e"
```
Use a `timeout`/watchdog around the daemon calls. `chmod +x`.

- [ ] **Step 2: Run it:**
```bash
bash tests/e2e_daemon_smoke.sh
```
Expected: all PASS (daemon serves, attach works, second serve refused, read-only enforced, `--stop` cleans up).

- [ ] **Step 3: Full regression (paste output):**
```bash
ctest --test-dir /tmp/ot-build-test --output-on-failure                    # all tst_* incl tst_serve_command
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli
APP=/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal
for t in tools datahub-peek feeds bridge-discovery paper portfolio-replication; do "$APP" --selftest-$t; echo "$t exit=$?"; done
# headless one-shot still works:
/tmp/ot-build-ht/openterminalcli --headless mcp list | head -c 80; echo
```
Expected: all green; no regression from the bridge `kind`/daemon additions.

- [ ] **Step 4: Phase-1 attach-to-GUI still works (best-effort)** — launch GUI, `openterminalcli status` shows `kind=gui` attached, `mcp call get_quote` works; `serve --stop` against the GUI is REFUSED ("owner is a gui"). If env blocks GUI automation, note it.

- [ ] **Step 5: Commit**
```bash
git add tests/e2e_daemon_smoke.sh
git commit -m "test(cli): end-to-end serve daemon lifecycle + regression"
```

---

## Self-Review
**Spec coverage:** single-owner refuse → T2 Step 2; HeadlessRuntime init + explicit bridge start + daemon mode → T2; `kind:"daemon"` → T1; read-only auth-checker (destructive deferred) → T2; SIGTERM clean shutdown removes bridge.json → T2 (self-pipe) + e2e T4; transparent attach (no client change) → T2/T4 (existing mcp/hub/quote unchanged); `--status`/`--stop` (kind-aware) → T3; deferred streaming/daemonize/destructive → explicitly out of scope. No-regression → T4.

**Known discovery points (verification-bounded, not placeholders):** (a) the exact `set_auth_checker` signature + the bridge's `cli::BridgeInfo` construction site → grep-named in T1/T2; (b) `HeadlessRuntime::init` doesn't start the bridge (stated; verify) — the daemon owns the start; (c) signal handling is POSIX (`#ifndef _WIN32`) — Windows daemon deferred.

**Type consistency:** `BridgeInfo::kind`, `set_owner_kind`, `serve_run/serve_status/serve_stop`, `is_settings_write_tool`, `AuthLevel::Verified` used identically across tasks and match the shipped 2a/2b names. Exit codes (0/2/3/5/7) match the spec/CLI scheme.

**Placeholder scan:** every code step carries complete code; the e2e tool names (`get_quote`, `set_setting`) are the verified real names.
