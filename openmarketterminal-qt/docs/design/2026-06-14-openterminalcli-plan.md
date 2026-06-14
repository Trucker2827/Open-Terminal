# openterminalcli Phase-1 (Attach Mode) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A headless `openterminalcli` binary that attaches to a running OpenTerminal GUI over its localhost `TerminalMcpBridge` and runs read-only/non-destructive commands (`status`, `mcp list/describe/call`, `hub topics/peek/request`, `quote`, `version`).

**Architecture:** A small shared, dependency-light module (`BridgeDiscoveryFile`) writes/reads a `bridge.json` `{endpoint, token, pid, started_at}` discovery file under the profile dir; the GUI bridge writes it on `start()` and removes it on `stop()`. A new `openterminalcli` target (links only `Qt6::Core` + `Qt6::Network`, no Widgets) reads that file, then speaks the bridge's HTTP/JSON (`GET /tools`, `POST /tool`, `X-MCP-Token` header) — never sending the destructive token. The CLI computes the discovery-file path by the same rule the GUI uses, so no GUI linkage is required.

**Tech Stack:** C++20, Qt6 (`Core`, `Network`), CMake + Ninja, QtTest (the repo's opt-in `OPENMARKETTERMINAL_BUILD_TESTS` harness), the project's `--selftest-*` one-shot idiom.

**Reference spec:** `openmarketterminal-qt/docs/design/2026-06-14-openterminalcli-design.md`

**Repo root for builds:** `~/src/Open-Terminal/openmarketterminal-qt` · **Dev build dir:** `/tmp/ot-build-ht` · **Test build dir (new):** `/tmp/ot-build-test` (configured with `-DOPENMARKETTERMINAL_BUILD_TESTS=ON`).

---

## Key facts (verified against the codebase)

- **Bridge protocol** (`src/mcp/TerminalMcpBridge.cpp`): HTTP/1.1 on `127.0.0.1:<ephemeral>`. Auth header `X-MCP-Token: <token_>`. Routes: `GET /tools` → `{ "tools":[{name,description,inputSchema,serverId},…] }`; `POST /tool` body `{id,tool,args,serverId?}` → `ToolResult.to_json()` with `id` echoed. `Connection: close`. Accessors: `endpoint()`, `token()`, `is_active()`, idempotent `start()`, `stop()`.
- **`ToolResult.to_json()`** (`src/mcp/McpTypes.h`): `{ "success": bool, "error"?: string, … }`.
- **Tool names/args:** `get_quote` → `{"symbol":"AAPL"}`; `datahub_list_topics` → `{}`; `datahub_peek` → `{"topic":"market:quote:AAPL"}`; `datahub_request` → `{"topic":…}`.
- **Discovery path:** `AppPaths::root()` (`src/core/config/AppPaths.cpp`) is a pure function: macOS `~/Library/Application Support/org.openterminal.OpenTerminal`, Linux `~/.local/share/org.openterminal.OpenTerminal`, Windows `<GenericDataLocation>/org.openterminal.OpenTerminal`. `ProfileManager::profile_root()` = `root()` for profile `"default"`, else `root()+"/profiles/"+name`.
- **Settings accessor** (`src/storage/repositories/SettingsRepository.h`, used at `main.cpp:148`): `SettingsRepository::instance().get(key, default)` returns a `Result` with `.is_ok()` and `.value()`.
- **Bridge is started only by `AgentService` today** (`src/services/agents/AgentService.cpp:89`) — Task 5 adds boot-time autostart.
- **`tests/` does not exist yet**; `CMakeLists.txt:3843` does `add_subdirectory(tests)` under `if(OPENMARKETTERMINAL_BUILD_TESTS)`. This plan creates `tests/`.

---

## File Structure

**Create:**
- `src/cli/BridgeDiscoveryFile.h` / `.cpp` — shared, Qt6::Core-only: the `BridgeInfo` struct, path replication, write/read/remove, PID-liveness. Compiled into **both** the GUI exe and `openterminalcli`.
- `src/cli/BridgeDiscovery.h` / `.cpp` — CLI-side resolver: `bridge.json` → live `{endpoint,token}` or typed error.
- `src/cli/BridgeClient.h` / `.cpp` — CLI-side HTTP client for the bridge.
- `src/cli/CommandDispatch.h` / `.cpp` — argv → command → client → formatted output.
- `src/cli/main.cpp` — `openterminalcli` entry point (`QCoreApplication`).
- `src/mcp/BridgeDiscoverySelftest.h` / `.cpp` — `--selftest-bridge-discovery` one-shot (GUI binary).
- `tests/CMakeLists.txt` — QtTest harness subdir.
- `tests/tst_bridge_discovery_file.cpp` — unit tests for `BridgeDiscoveryFile`.
- `tests/tst_bridge_client.cpp` — unit tests for `BridgeClient` against an in-process fake server.

**Modify:**
- `src/mcp/TerminalMcpBridge.h` / `.cpp` — write discovery file on `start()`, remove on `stop()`.
- `src/app/main.cpp` — boot-time `bridge.autostart`; dispatch `--selftest-bridge-discovery`; remove discovery file on `aboutToQuit`.
- `CMakeLists.txt` — add `BridgeDiscoveryFile.cpp`/`BridgeDiscoverySelftest.cpp` to the GUI build; new `openterminalcli` target; wire `tests/` subdir.

---

## Task 1: `BridgeDiscoveryFile` — shared discovery module (path + read/write)

**Files:**
- Create: `src/cli/BridgeDiscoveryFile.h`, `src/cli/BridgeDiscoveryFile.cpp`
- Create: `tests/CMakeLists.txt`, `tests/tst_bridge_discovery_file.cpp`
- Modify: `CMakeLists.txt` (add the module to a shared var; the GUI already builds; tests subdir already wired)

- [ ] **Step 1: Write the header**

Create `src/cli/BridgeDiscoveryFile.h`:

```cpp
#pragma once
#include <QString>
#include <optional>

namespace openmarketterminal::cli {

struct BridgeInfo {
    QString endpoint;    // "http://127.0.0.1:<port>"
    QString token;       // X-MCP-Token value
    qint64  pid = 0;     // PID of the GUI that owns the bridge
    QString started_at;  // ISO-8601 UTC
};

/// Replicates AppPaths::root()+ProfileManager::profile_root() WITHOUT linking
/// the GUI. Must stay byte-identical to those functions.
QString profile_root_for(const QString& profile);

/// "<profile_root>/bridge.json"
QString bridge_file_path(const QString& profile_root);

/// Write bridge.json (pretty JSON) with 0600 perms. Returns false on failure.
bool write_bridge_file(const QString& profile_root, const BridgeInfo& info);

/// Remove bridge.json if present. Returns true if absent or successfully removed.
bool remove_bridge_file(const QString& profile_root);

/// Parse bridge.json. Returns nullopt if missing, unreadable, or malformed.
std::optional<BridgeInfo> read_bridge_file(const QString& profile_root);

/// True if a process with this PID currently exists (kill(pid,0) / Win OpenProcess).
bool is_pid_alive(qint64 pid);

} // namespace openmarketterminal::cli
```

- [ ] **Step 2: Write the failing tests**

Create `tests/tst_bridge_discovery_file.cpp`:

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "cli/BridgeDiscoveryFile.h"

using namespace openmarketterminal::cli;

class TstBridgeDiscoveryFile : public QObject {
    Q_OBJECT
private slots:
    void profile_root_default_matches_literal() {
        const QString r = profile_root_for("default");
        QVERIFY(r.endsWith("org.openterminal.OpenTerminal"));
        QVERIFY(!r.contains("/profiles/"));
    }
    void profile_root_named_appends_profiles() {
        const QString r = profile_root_for("alice");
        QVERIFY(r.endsWith("org.openterminal.OpenTerminal/profiles/alice"));
    }
    void write_then_read_roundtrips() {
        QTemporaryDir dir;
        BridgeInfo in{"http://127.0.0.1:54923", "tok-123", 4242, "2026-06-14T00:00:00Z"};
        QVERIFY(write_bridge_file(dir.path(), in));
        auto out = read_bridge_file(dir.path());
        QVERIFY(out.has_value());
        QCOMPARE(out->endpoint, in.endpoint);
        QCOMPARE(out->token, in.token);
        QCOMPARE(out->pid, in.pid);
    }
    void file_is_owner_only() {
        QTemporaryDir dir;
        write_bridge_file(dir.path(), {"http://127.0.0.1:1", "t", 1, "x"});
        const auto perms = QFile::permissions(bridge_file_path(dir.path()));
        QVERIFY(!(perms & (QFile::ReadGroup | QFile::ReadOther |
                           QFile::WriteGroup | QFile::WriteOther)));
    }
    void read_missing_is_nullopt() {
        QTemporaryDir dir;
        QVERIFY(!read_bridge_file(dir.path()).has_value());
    }
    void read_malformed_is_nullopt() {
        QTemporaryDir dir;
        QFile f(bridge_file_path(dir.path()));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write("not json{"); f.close();
        QVERIFY(!read_bridge_file(dir.path()).has_value());
    }
    void remove_is_idempotent() {
        QTemporaryDir dir;
        QVERIFY(remove_bridge_file(dir.path()));            // absent → true
        write_bridge_file(dir.path(), {"e","t",1,"x"});
        QVERIFY(remove_bridge_file(dir.path()));            // present → removed
        QVERIFY(!QFile::exists(bridge_file_path(dir.path())));
    }
    void self_pid_is_alive() {
        QVERIFY(is_pid_alive(QCoreApplication::applicationPid()));
        QVERIFY(!is_pid_alive(999999999));
    }
};
QTEST_MAIN(TstBridgeDiscoveryFile)
#include "tst_bridge_discovery_file.moc"
```

- [ ] **Step 3: Create the tests CMake subdir**

Create `tests/CMakeLists.txt`:

```cmake
# QtTest suite (built only when OPENMARKETTERMINAL_BUILD_TESTS=ON).
find_package(Qt6 REQUIRED COMPONENTS Test Network)

add_executable(tst_bridge_discovery_file
    tst_bridge_discovery_file.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_discovery_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_discovery_file PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_bridge_discovery_file COMMAND tst_bridge_discovery_file)
```

- [ ] **Step 4: Configure the test build and run — verify it FAILS to compile/link**

```bash
cd ~/src/Open-Terminal/openmarketterminal-qt
cmake -S . -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENMARKETTERMINAL_BUILD_TESTS=ON
cmake --build /tmp/ot-build-test --target tst_bridge_discovery_file
```
Expected: **build FAILS** — `BridgeDiscoveryFile.cpp` does not exist yet.

- [ ] **Step 5: Implement `BridgeDiscoveryFile.cpp`**

Create `src/cli/BridgeDiscoveryFile.cpp`:

```cpp
#include "cli/BridgeDiscoveryFile.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <csignal>
#endif

namespace openmarketterminal::cli {

// Mirror of AppPaths::root() — keep identical to src/core/config/AppPaths.cpp.
static QString app_root() {
#if defined(Q_OS_WIN)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + "/org.openterminal.OpenTerminal";
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Application Support/org.openterminal.OpenTerminal";
#else
    return QDir::homePath() + "/.local/share/org.openterminal.OpenTerminal";
#endif
}

QString profile_root_for(const QString& profile) {
    return (profile.isEmpty() || profile == "default")
               ? app_root()
               : app_root() + "/profiles/" + profile;
}

QString bridge_file_path(const QString& profile_root) {
    return profile_root + "/bridge.json";
}

bool write_bridge_file(const QString& profile_root, const BridgeInfo& info) {
    QDir().mkpath(profile_root);
    QJsonObject o;
    o["schema"] = 1;
    o["endpoint"] = info.endpoint;
    o["token"] = info.token;
    o["pid"] = info.pid;
    o["started_at"] = info.started_at;
    const QString path = bridge_file_path(profile_root);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

bool remove_bridge_file(const QString& profile_root) {
    const QString path = bridge_file_path(profile_root);
    if (!QFile::exists(path))
        return true;
    return QFile::remove(path);
}

std::optional<BridgeInfo> read_bridge_file(const QString& profile_root) {
    QFile f(bridge_file_path(profile_root));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();
    if (!o.contains("endpoint") || !o.contains("token"))
        return std::nullopt;
    BridgeInfo info;
    info.endpoint = o.value("endpoint").toString();
    info.token = o.value("token").toString();
    info.pid = static_cast<qint64>(o.value("pid").toDouble());
    info.started_at = o.value("started_at").toString();
    if (info.endpoint.isEmpty() || info.token.isEmpty())
        return std::nullopt;
    return info;
}

bool is_pid_alive(qint64 pid) {
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0; GetExitCodeProcess(h, &code); CloseHandle(h);
    return code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace openmarketterminal::cli
```

- [ ] **Step 6: Build and run the tests — verify PASS**

```bash
cmake --build /tmp/ot-build-test --target tst_bridge_discovery_file
ctest --test-dir /tmp/ot-build-test -R tst_bridge_discovery_file --output-on-failure
```
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 7: Commit**

```bash
git add src/cli/BridgeDiscoveryFile.h src/cli/BridgeDiscoveryFile.cpp \
        tests/CMakeLists.txt tests/tst_bridge_discovery_file.cpp
git commit -m "feat(cli): shared bridge.json discovery module + unit tests"
```

---

## Task 2: Bridge writes/removes the discovery file (GUI side)

**Files:**
- Modify: `src/mcp/TerminalMcpBridge.h` (add `discovery_root_` member), `src/mcp/TerminalMcpBridge.cpp` (`start()`/`stop()`)
- Modify: `CMakeLists.txt` (compile `src/cli/BridgeDiscoveryFile.cpp` into the GUI exe)

- [ ] **Step 1: Add the discovery write to `start()`**

In `src/mcp/TerminalMcpBridge.cpp`, add includes near the top:

```cpp
#include "cli/BridgeDiscoveryFile.h"
#include "core/config/ProfileManager.h"
#include <QCoreApplication>
#include <QDateTime>
```

In `start()`, immediately after `active_ = true;` (currently line ~87), insert:

```cpp
    discovery_root_ = ProfileManager::instance().profile_root();
    cli::write_bridge_file(discovery_root_, cli::BridgeInfo{
        endpoint(), token_, QCoreApplication::applicationPid(),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate)});
```

- [ ] **Step 2: Remove the file in `stop()`**

In `stop()`, after `active_ = false;` (line ~95), insert:

```cpp
    if (!discovery_root_.isEmpty()) {
        cli::remove_bridge_file(discovery_root_);
        discovery_root_.clear();
    }
```

- [ ] **Step 3: Add the member to the header**

In `src/mcp/TerminalMcpBridge.h`, in the private members block (near `QString token_;`), add:

```cpp
    QString discovery_root_;  // profile root where bridge.json was written
```

- [ ] **Step 4: Wire `BridgeDiscoveryFile.cpp` into the GUI exe**

In `CMakeLists.txt`, find the `set(MCP_SOURCES` block (line ~971) and add as its first entry (after the opening line):

```cmake
    src/cli/BridgeDiscoveryFile.cpp
```

- [ ] **Step 5: Add a `--selftest-bridge-discovery` one-shot**

Create `src/mcp/BridgeDiscoverySelftest.h`:

```cpp
#pragma once
namespace openmarketterminal::mcp { int run_bridge_discovery_selftest(); }
```

Create `src/mcp/BridgeDiscoverySelftest.cpp`:

```cpp
#include "mcp/BridgeDiscoverySelftest.h"
#include "mcp/TerminalMcpBridge.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/config/ProfileManager.h"
#include <cstdio>

namespace openmarketterminal::mcp {

int run_bridge_discovery_selftest() {
    auto& bridge = TerminalMcpBridge::instance();
    const QString root = ProfileManager::instance().profile_root();
    cli::remove_bridge_file(root);

    if (!bridge.start()) { std::fprintf(stderr, "FAIL: bridge.start()\n"); return 1; }
    auto info = cli::read_bridge_file(root);
    if (!info) { std::fprintf(stderr, "FAIL: bridge.json not written\n"); return 1; }
    if (info->endpoint != bridge.endpoint() || info->token != bridge.token()) {
        std::fprintf(stderr, "FAIL: bridge.json mismatch\n"); return 1;
    }
    bridge.stop();
    if (cli::read_bridge_file(root)) { std::fprintf(stderr, "FAIL: bridge.json not removed on stop\n"); return 1; }
    std::printf("OK: bridge discovery write/remove\n");
    return 0;
}

} // namespace openmarketterminal::mcp
```

Add `src/mcp/BridgeDiscoverySelftest.cpp` to the `MCP_SOURCES` block in `CMakeLists.txt` (next to `ToolSelfTest.cpp`, line ~981).

In `src/app/main.cpp`, add the include near the other selftest includes (line ~31):

```cpp
#include "mcp/BridgeDiscoverySelftest.h"
```

and add the dispatch next to the other `--selftest-*` checks (after line ~816):

```cpp
        if (qstrcmp(argv[i], "--selftest-bridge-discovery") == 0)
            return openmarketterminal::mcp::run_bridge_discovery_selftest();
```

- [ ] **Step 6: Build and run the selftest — verify PASS**

```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal
/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-bridge-discovery; echo "exit=$?"
```
Expected: `OK: bridge discovery write/remove` then `exit=0`.

- [ ] **Step 7: Commit**

```bash
git add src/mcp/TerminalMcpBridge.h src/mcp/TerminalMcpBridge.cpp \
        src/mcp/BridgeDiscoverySelftest.h src/mcp/BridgeDiscoverySelftest.cpp \
        src/app/main.cpp CMakeLists.txt
git commit -m "feat(bridge): advertise endpoint+token via bridge.json on start/stop"
```

---

## Task 3: Boot-time bridge autostart + cleanup on quit (GUI side)

**Files:**
- Modify: `src/app/main.cpp`

- [ ] **Step 1: Start the bridge at boot, gated by a setting**

In `src/app/main.cpp`, find the block where services register with the hub (the `ensure_registered_with_hub()` calls around line ~320–340). After the last such call, insert:

```cpp
    // Bridge autostart: make the localhost tool surface available whenever the
    // GUI is up (not only during an agent run), so openterminalcli can attach.
    {
        const auto r = openmarketterminal::SettingsRepository::instance().get(
            QStringLiteral("bridge.autostart"), QStringLiteral("true"));
        const QString v = r.is_ok() ? r.value() : QStringLiteral("true");
        if (v != QStringLiteral("false"))
            openmarketterminal::mcp::TerminalMcpBridge::instance().start();
    }
```

Add the include near the MCP includes (line ~29) if not already present:

```cpp
#include "mcp/TerminalMcpBridge.h"
```

- [ ] **Step 2: Remove the discovery file on clean quit**

In `src/app/main.cpp`, the `QObject::connect(&app, &QCoreApplication::aboutToQuit, …)` block (line ~296) — add inside the lambda body:

```cpp
        openmarketterminal::mcp::TerminalMcpBridge::instance().stop();
```

(`stop()` removes `bridge.json`; it is a no-op if the bridge never started.)

- [ ] **Step 3: Build and verify the file appears while running, gone after quit**

```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal
ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
rm -f "$ROOT/bridge.json"
open /tmp/ot-build-ht/OpenTerminal.app
sleep 6
test -f "$ROOT/bridge.json" && echo "PRESENT while running ✓" && stat -f "%Sp" "$ROOT/bridge.json"
osascript -e 'quit app "OpenTerminal"' 2>/dev/null; sleep 2
test -f "$ROOT/bridge.json" && echo "STILL PRESENT (bug)" || echo "REMOVED on quit ✓"
```
Expected: `PRESENT while running ✓`, perms `-rw-------`, then `REMOVED on quit ✓`.

- [ ] **Step 4: Commit**

```bash
git add src/app/main.cpp
git commit -m "feat(app): autostart the MCP bridge at boot (bridge.autostart, default on)"
```

---

## Task 4: `openterminalcli` target + entry point + global flags

**Files:**
- Create: `src/cli/main.cpp`, `src/cli/CommandDispatch.h`, `src/cli/CommandDispatch.cpp`
- Modify: `CMakeLists.txt` (new `openterminalcli` executable)

- [ ] **Step 1: Write the dispatch header (with the parsed-options struct)**

Create `src/cli/CommandDispatch.h`:

```cpp
#pragma once
#include <QStringList>

namespace openmarketterminal::cli {

struct GlobalOpts {
    bool json = false;
    QString profile = "default";
};

// Parse [--json] [--profile X] off the FRONT of args; mutates args to the
// remaining <group> <command> [params]. Returns false on a bad --profile.
bool parse_global_opts(QStringList& args, GlobalOpts& out);

// Entry: returns a process exit code (see spec §4). Prints data to stdout,
// diagnostics to stderr.
int dispatch(QStringList args);

} // namespace openmarketterminal::cli
```

- [ ] **Step 2: Write the failing test for option parsing**

Append to `tests/tst_bridge_discovery_file.cpp` is wrong target; instead create `tests/tst_command_dispatch.cpp`:

```cpp
#include <QtTest>
#include "cli/CommandDispatch.h"
using namespace openmarketterminal::cli;

class TstCommandDispatch : public QObject {
    Q_OBJECT
private slots:
    void strips_global_flags() {
        QStringList args{"--json", "--profile", "alice", "mcp", "list"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(o.json);
        QCOMPARE(o.profile, QString("alice"));
        QCOMPARE(args, (QStringList{"mcp", "list"}));
    }
    void defaults_when_absent() {
        QStringList args{"status"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(!o.json);
        QCOMPARE(o.profile, QString("default"));
        QCOMPARE(args, (QStringList{"status"}));
    }
    void rejects_dangling_profile() {
        QStringList args{"--profile"};
        GlobalOpts o;
        QVERIFY(!parse_global_opts(args, o));
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_command_dispatch
    tst_command_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CommandDispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscovery.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_command_dispatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_command_dispatch PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME tst_command_dispatch COMMAND tst_command_dispatch)
```

(The `BridgeDiscovery.cpp`/`BridgeClient.cpp` sources are created in Tasks 5–6; this test target compiles once those exist. Build it at the end of Task 6.)

- [ ] **Step 3: Implement option parsing + a minimal dispatch (`version`, usage)**

Create `src/cli/CommandDispatch.cpp` (commands filled in Tasks 5,7,8 — for now `version`/usage only):

```cpp
#include "cli/CommandDispatch.h"
#include <QCoreApplication>
#include <cstdio>

namespace openmarketterminal::cli {

bool parse_global_opts(QStringList& args, GlobalOpts& out) {
    while (!args.isEmpty() && args.first().startsWith("--")) {
        const QString flag = args.takeFirst();
        if (flag == "--json") { out.json = true; }
        else if (flag == "--profile") {
            if (args.isEmpty()) return false;
            out.profile = args.takeFirst();
        } else { args.prepend(flag); break; } // unknown global flag → stop
    }
    return true;
}

static int usage() {
    std::fprintf(stderr,
        "openterminalcli [--json] [--profile <name>] <group> <command> [args]\n"
        "  status\n  version\n"
        "  mcp list | describe <tool> | call <tool> '<json>'\n"
        "  hub topics | peek <topic> | request <topic> [k=v...]\n"
        "  quote <SYM...>\n");
    return 2;
}

int dispatch(QStringList args) {
    GlobalOpts opts;
    if (!parse_global_opts(args, opts)) { std::fprintf(stderr, "error: --profile requires a value\n"); return 2; }
    if (args.isEmpty()) return usage();

    const QString group = args.takeFirst();
    if (group == "version") {
        std::printf("openterminalcli %s\n", qUtf8Printable(QCoreApplication::applicationVersion()));
        return 0;
    }
    // status / mcp / hub / quote wired in Tasks 5,7,8.
    std::fprintf(stderr, "error: unknown command '%s'\n", qUtf8Printable(group));
    return 2;
}

} // namespace openmarketterminal::cli
```

- [ ] **Step 4: Write the entry point**

Create `src/cli/main.cpp`:

```cpp
#include "cli/CommandDispatch.h"
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("openterminalcli");
    app.setApplicationVersion(OPENMARKETTERMINAL_VERSION_STRING);
    QStringList args = app.arguments();
    args.removeFirst(); // drop argv[0]
    return openmarketterminal::cli::dispatch(args);
}
```

- [ ] **Step 5: Add the target to CMake**

In `CMakeLists.txt`, after the `OpenMarketTerminal` target's `target_link_libraries` block (after line ~2864), add:

```cmake
# ── openterminalcli — headless attach client (Qt6::Core + Network only) ──────
add_executable(openterminalcli
    src/cli/main.cpp
    src/cli/CommandDispatch.cpp
    src/cli/BridgeDiscovery.cpp
    src/cli/BridgeClient.cpp
    src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(openterminalcli PRIVATE src)
target_compile_definitions(openterminalcli PRIVATE
    OPENMARKETTERMINAL_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(openterminalcli PRIVATE Qt6::Core Qt6::Network)
```

(`BridgeDiscovery.cpp`/`BridgeClient.cpp` are created in Tasks 5–6; the target links once they exist — build it at the end of Task 6.)

- [ ] **Step 6: Commit (compiles after Task 6; verify parse test wiring now)**

```bash
git add src/cli/main.cpp src/cli/CommandDispatch.h src/cli/CommandDispatch.cpp \
        tests/tst_command_dispatch.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(cli): openterminalcli target, entry point, global flag parsing"
```

---

## Task 5: `BridgeDiscovery` — resolve a running instance

**Files:**
- Create: `src/cli/BridgeDiscovery.h`, `src/cli/BridgeDiscovery.cpp`
- Modify: `src/cli/CommandDispatch.cpp` (add `status`)

- [ ] **Step 1: Write the header**

Create `src/cli/BridgeDiscovery.h`:

```cpp
#pragma once
#include "cli/BridgeDiscoveryFile.h"
#include <QString>
#include <variant>

namespace openmarketterminal::cli {

enum class DiscoveryError { NotRunning, Stale };

struct Discovered { BridgeInfo info; };

// Read bridge.json for `profile`; verify the owning PID is alive.
std::variant<Discovered, DiscoveryError> resolve(const QString& profile);

const char* describe(DiscoveryError e);

} // namespace openmarketterminal::cli
```

- [ ] **Step 2: Implement**

Create `src/cli/BridgeDiscovery.cpp`:

```cpp
#include "cli/BridgeDiscovery.h"

namespace openmarketterminal::cli {

std::variant<Discovered, DiscoveryError> resolve(const QString& profile) {
    const QString root = profile_root_for(profile);
    auto info = read_bridge_file(root);
    if (!info)
        return DiscoveryError::NotRunning;
    if (!is_pid_alive(info->pid))
        return DiscoveryError::Stale;
    return Discovered{*info};
}

const char* describe(DiscoveryError e) {
    switch (e) {
        case DiscoveryError::NotRunning: return "No running OpenTerminal instance for this profile. Start the app (or check 'bridge.autostart').";
        case DiscoveryError::Stale:      return "Stale bridge.json (owning process is gone). Restart OpenTerminal.";
    }
    return "Unknown discovery error";
}

} // namespace openmarketterminal::cli
```

- [ ] **Step 3: Add `status` to dispatch**

In `src/cli/CommandDispatch.cpp`, add `#include "cli/BridgeDiscovery.h"` and `#include <QJsonObject>`/`#include <QJsonDocument>` at the top, then in `dispatch()` before the unknown-command fallback:

```cpp
    if (group == "status") {
        auto r = resolve(opts.profile);
        if (auto* d = std::get_if<Discovered>(&r)) {
            if (opts.json) {
                QJsonObject o{{"attached", true}, {"endpoint", d->info.endpoint}, {"pid", d->info.pid}};
                std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
            } else {
                std::printf("attached  endpoint=%s  pid=%lld\n",
                            qUtf8Printable(d->info.endpoint), static_cast<long long>(d->info.pid));
            }
            return 0;
        }
        std::fprintf(stderr, "%s\n", describe(std::get<DiscoveryError>(r)));
        return 3;
    }
```

- [ ] **Step 4: Write the failing test**

Add to `tests/tst_command_dispatch.cpp` (new slots) — uses a temp HOME so `profile_root_for` points into a sandbox:

```cpp
    void resolve_missing_is_not_running() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<DiscoveryError>(r));
        QCOMPARE(std::get<DiscoveryError>(r), DiscoveryError::NotRunning);
    }
    void resolve_live_pid_succeeds() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        write_bridge_file(profile_root_for("default"),
            {"http://127.0.0.1:9", "tok", QCoreApplication::applicationPid(), "x"});
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<Discovered>(r));
    }
```

Add `#include "cli/BridgeDiscovery.h"` and `#include <QTemporaryDir>` to that test file's includes.

- [ ] **Step 5: Build & run**

```bash
cmake --build /tmp/ot-build-test --target tst_command_dispatch
ctest --test-dir /tmp/ot-build-test -R tst_command_dispatch --output-on-failure
```
Expected: PASS (note: HOME override affects only the test process).

- [ ] **Step 6: Commit**

```bash
git add src/cli/BridgeDiscovery.h src/cli/BridgeDiscovery.cpp \
        src/cli/CommandDispatch.cpp tests/tst_command_dispatch.cpp
git commit -m "feat(cli): BridgeDiscovery + status command"
```

---

## Task 6: `BridgeClient` — HTTP to the bridge

**Files:**
- Create: `src/cli/BridgeClient.h`, `src/cli/BridgeClient.cpp`
- Modify: `tests/tst_bridge_client.cpp` (new), `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/cli/BridgeClient.h`:

```cpp
#pragma once
#include "cli/BridgeDiscoveryFile.h"
#include <QJsonObject>
#include <QJsonArray>

namespace openmarketterminal::cli {

enum class ClientStatus { Ok, Unauthorized, Transport, BadResponse };

struct ClientResult {
    ClientStatus status = ClientStatus::Transport;
    QJsonObject body;     // parsed JSON (tool result or {tools:[...]})
    QString error;        // human message when !Ok
};

class BridgeClient {
public:
    explicit BridgeClient(BridgeInfo info) : info_(std::move(info)) {}

    ClientResult get_tools();                                   // GET /tools
    ClientResult call_tool(const QString& name, const QJsonObject& args); // POST /tool

private:
    ClientResult request(const QByteArray& method, const QString& path, const QByteArray& body);
    BridgeInfo info_;
};

} // namespace openmarketterminal::cli
```

- [ ] **Step 2: Write the failing test (in-process fake bridge)**

Create `tests/tst_bridge_client.cpp`:

```cpp
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include "cli/BridgeClient.h"
using namespace openmarketterminal::cli;

// Minimal fake of TerminalMcpBridge: checks X-MCP-Token, answers /tools and /tool.
class FakeBridge : public QObject {
    Q_OBJECT
public:
    quint16 port = 0;
    QString token = "secret";
    FakeBridge() {
        connect(&srv_, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* s = srv_.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, this, [this, s] {
                const QByteArray req = s->readAll();
                const bool authed = req.contains("X-MCP-Token: " + token.toUtf8());
                QByteArray body; int code = 200;
                if (!authed) { code = 401; body = R"({"success":false,"error":"bad token"})"; }
                else if (req.startsWith("GET /tools")) body = R"({"tools":[{"name":"get_quote"}]})";
                else if (req.startsWith("POST /tool")) {
                    body = req.contains("\"fail\"") ? R"({"success":false,"error":"nope"})"
                                                    : R"({"success":true})";
                }
                QByteArray resp = "HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                    "Connection: close\r\n\r\n" + body;
                s->write(resp); s->flush(); s->disconnectFromHost();
            });
        });
        srv_.listen(QHostAddress::LocalHost, 0);
        port = srv_.serverPort();
    }
private:
    QTcpServer srv_;
};

class TstBridgeClient : public QObject {
    Q_OBJECT
private slots:
    void get_tools_ok() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.get_tools();
        QCOMPARE(r.status, ClientStatus::Ok);
        QVERIFY(r.body.value("tools").toArray().size() == 1);
    }
    void bad_token_is_unauthorized() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "WRONG", 0, ""});
        QCOMPARE(c.get_tools().status, ClientStatus::Unauthorized);
    }
    void call_tool_failure_parses() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.call_tool("x", QJsonObject{{"fail", true}});
        QCOMPARE(r.status, ClientStatus::Ok);                 // transport ok…
        QCOMPARE(r.body.value("success").toBool(), false);    // …tool reported failure
    }
    void dead_endpoint_is_transport() {
        BridgeClient c({"http://127.0.0.1:1", "secret", 0, ""});
        QCOMPARE(c.get_tools().status, ClientStatus::Transport);
    }
};
QTEST_MAIN(TstBridgeClient)
#include "tst_bridge_client.moc"
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_bridge_client
    tst_bridge_client.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_client PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME tst_bridge_client COMMAND tst_bridge_client)
```

- [ ] **Step 3: Implement `BridgeClient.cpp`**

Create `src/cli/BridgeClient.cpp` (synchronous via `QTcpSocket` — simplest for a one-shot CLI; the bridge sends `Connection: close`):

```cpp
#include "cli/BridgeClient.h"
#include <QJsonDocument>
#include <QTcpSocket>
#include <QUrl>

namespace openmarketterminal::cli {

ClientResult BridgeClient::request(const QByteArray& method, const QString& path, const QByteArray& body) {
    const QUrl url(info_.endpoint);
    QTcpSocket sock;
    sock.connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    if (!sock.waitForConnected(3000))
        return {ClientStatus::Transport, {}, "cannot connect to " + info_.endpoint};

    QByteArray req = method + " " + path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "X-MCP-Token: " + info_.token.toUtf8() + "\r\n";
    req += "Connection: close\r\n";
    if (!body.isEmpty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    sock.write(req);
    if (!sock.waitForBytesWritten(3000))
        return {ClientStatus::Transport, {}, "write failed"};

    QByteArray resp;
    while (sock.state() == QAbstractSocket::ConnectedState && sock.waitForReadyRead(5000))
        resp += sock.readAll();
    resp += sock.readAll();

    const int hdr_end = resp.indexOf("\r\n\r\n");
    if (hdr_end < 0)
        return {ClientStatus::BadResponse, {}, "no header terminator"};
    const QByteArray status_line = resp.left(resp.indexOf("\r\n"));
    const QByteArray payload = resp.mid(hdr_end + 4);
    const int code = status_line.split(' ').value(1).toInt();

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};

    if (code == 401)
        return {ClientStatus::Unauthorized, obj, "bridge rejected token (restart? re-read bridge.json)"};
    if (code != 200)
        return {ClientStatus::BadResponse, obj, "HTTP " + QString::number(code)};
    if (!doc.isObject())
        return {ClientStatus::BadResponse, {}, "non-JSON response"};
    return {ClientStatus::Ok, obj, {}};
}

ClientResult BridgeClient::get_tools() {
    return request("GET", "/tools", {});
}

ClientResult BridgeClient::call_tool(const QString& name, const QJsonObject& args) {
    QJsonObject body{{"tool", name}, {"args", args}};
    return request("POST", "/tool", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace openmarketterminal::cli
```

- [ ] **Step 4: Build & run the client tests**

```bash
cmake --build /tmp/ot-build-test --target tst_bridge_client
ctest --test-dir /tmp/ot-build-test -R tst_bridge_client --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 5: Build the full CLI + dispatch test (now that all sources exist)**

```bash
cmake --build /tmp/ot-build-test --target tst_command_dispatch
ctest --test-dir /tmp/ot-build-test -R "tst_" --output-on-failure
cmake --build /tmp/ot-build-ht --target openterminalcli
/tmp/ot-build-ht/openterminalcli version    # prints version, exit 0
```
Expected: all `tst_*` pass; `openterminalcli version` prints a version string.

- [ ] **Step 6: Commit**

```bash
git add src/cli/BridgeClient.h src/cli/BridgeClient.cpp \
        tests/tst_bridge_client.cpp tests/CMakeLists.txt
git commit -m "feat(cli): BridgeClient HTTP/JSON to the bridge + tests"
```

---

## Task 7: `mcp list | describe | call`

**Files:**
- Modify: `src/cli/CommandDispatch.cpp`

- [ ] **Step 1: Add a shared helper that resolves + builds a client**

In `src/cli/CommandDispatch.cpp`, add `#include "cli/BridgeClient.h"` and a helper above `dispatch()`:

```cpp
// Resolve the running instance into a client, or print + return an exit code.
static bool make_client(const GlobalOpts& opts, BridgeClient*& out, int& exit_code) {
    auto r = resolve(opts.profile);
    if (auto* d = std::get_if<Discovered>(&r)) { out = new BridgeClient(d->info); return true; }
    std::fprintf(stderr, "%s\n", describe(std::get<DiscoveryError>(r)));
    exit_code = 3; return false;
}

// Map a ClientResult to (printed output + exit code). `data_key` empty = print whole body.
static int emit(const GlobalOpts& opts, const ClientResult& r) {
    if (r.status == ClientStatus::Unauthorized) { std::fprintf(stderr, "%s\n", qUtf8Printable(r.error)); return 4; }
    if (r.status != ClientStatus::Ok)           { std::fprintf(stderr, "%s\n", qUtf8Printable(r.error)); return 6; }
    if (r.body.contains("success") && !r.body.value("success").toBool()) {
        std::fprintf(stderr, "tool error: %s\n", qUtf8Printable(r.body.value("error").toString("(no message)")));
        return 5;
    }
    std::printf("%s\n", QJsonDocument(r.body).toJson(
        opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
    return 0;
}
```

- [ ] **Step 2: Wire the `mcp` group into `dispatch()`**

Add before the unknown-command fallback in `dispatch()`:

```cpp
    if (group == "mcp") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        BridgeClient* c = nullptr; int code = 0;
        if (!make_client(opts, c, code)) return code;

        if (sub == "list") {
            auto r = c->get_tools(); delete c; return emit(opts, r);
        }
        if (sub == "describe") {
            if (args.isEmpty()) { delete c; std::fprintf(stderr, "usage: mcp describe <tool>\n"); return 2; }
            const QString want = args.first();
            auto r = c->get_tools(); delete c;
            if (r.status != ClientStatus::Ok) return emit(opts, r);
            for (const auto v : r.body.value("tools").toArray()) {
                if (v.toObject().value("name").toString() == want) {
                    std::printf("%s\n", QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented).constData());
                    return 0;
                }
            }
            std::fprintf(stderr, "no such tool: %s\n", qUtf8Printable(want)); return 5;
        }
        if (sub == "call") {
            if (args.size() < 1) { delete c; std::fprintf(stderr, "usage: mcp call <tool> '<json>'\n"); return 2; }
            const QString tool = args.takeFirst();
            QJsonObject a;
            if (!args.isEmpty()) {
                const QJsonDocument d = QJsonDocument::fromJson(args.first().toUtf8());
                if (!d.isObject()) { delete c; std::fprintf(stderr, "args must be a JSON object\n"); return 2; }
                a = d.object();
            }
            auto r = c->call_tool(tool, a); delete c; return emit(opts, r);
        }
        delete c; std::fprintf(stderr, "usage: mcp list|describe|call\n"); return 2;
    }
```

- [ ] **Step 3: Integration check against a running app**

```bash
open /tmp/ot-build-ht/OpenTerminal.app; sleep 6
/tmp/ot-build-ht/openterminalcli mcp list | head -c 200; echo
/tmp/ot-build-ht/openterminalcli --json mcp call get_quote '{"symbol":"AAPL"}'; echo "exit=$?"
```
Expected: `mcp list` prints a JSON tool catalog; the `call` prints a JSON quote and `exit=0`.

- [ ] **Step 4: Commit**

```bash
git add src/cli/CommandDispatch.cpp
git commit -m "feat(cli): mcp list/describe/call"
```

---

## Task 8: Aliases — `hub topics|peek|request`, `quote`

**Files:**
- Modify: `src/cli/CommandDispatch.cpp`

- [ ] **Step 1: Add the alias commands to `dispatch()`**

Add before the unknown-command fallback:

```cpp
    if (group == "quote") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: quote <SYM...>\n"); return 2; }
        BridgeClient* c = nullptr; int code = 0;
        if (!make_client(opts, c, code)) return code;
        int rc = 0;
        for (const QString& sym : args) {
            auto r = c->call_tool("get_quote", QJsonObject{{"symbol", sym}});
            const int e = emit(opts, r);
            if (e != 0) rc = e;
        }
        delete c; return rc;
    }

    if (group == "hub") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        BridgeClient* c = nullptr; int code = 0;
        if (!make_client(opts, c, code)) return code;
        ClientResult r;
        if (sub == "topics") {
            r = c->call_tool("datahub_list_topics", {});
        } else if (sub == "peek" || sub == "request") {
            if (args.isEmpty()) { delete c; std::fprintf(stderr, "usage: hub %s <topic>\n", qUtf8Printable(sub)); return 2; }
            const QString tool = (sub == "peek") ? "datahub_peek" : "datahub_request";
            r = c->call_tool(tool, QJsonObject{{"topic", args.first()}});
        } else {
            delete c; std::fprintf(stderr, "usage: hub topics|peek|request\n"); return 2;
        }
        delete c; return emit(opts, r);
    }
```

- [ ] **Step 2: Test the alias→tool mapping (unit, against the fake bridge)**

Add a slot to `tests/tst_bridge_client.cpp` that asserts `call_tool` issues the right tool name by having the fake echo the request. Replace the fake's `/tool` branch to echo the body:

```cpp
                else if (req.startsWith("POST /tool")) {
                    const int b = req.indexOf("\r\n\r\n");
                    const QByteArray sent = req.mid(b + 4);
                    body = "{\"success\":true,\"echo\":" + sent + "}";
                }
```

and add:

```cpp
    void call_tool_sends_name_and_args() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.call_tool("get_quote", QJsonObject{{"symbol","AAPL"}});
        QCOMPARE(r.status, ClientStatus::Ok);
        const QJsonObject echo = r.body.value("echo").toObject();
        QCOMPARE(echo.value("tool").toString(), QString("get_quote"));
        QCOMPARE(echo.value("args").toObject().value("symbol").toString(), QString("AAPL"));
    }
```

- [ ] **Step 3: Build & run**

```bash
cmake --build /tmp/ot-build-test --target tst_bridge_client
ctest --test-dir /tmp/ot-build-test -R tst_bridge_client --output-on-failure
cmake --build /tmp/ot-build-ht --target openterminalcli
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/cli/CommandDispatch.cpp tests/tst_bridge_client.cpp
git commit -m "feat(cli): hub topics/peek/request + quote aliases"
```

---

## Task 9: End-to-end integration smoke (real GUI ⇄ real CLI)

**Files:**
- Create: `tests/e2e_attach_smoke.sh`

- [ ] **Step 1: Write the smoke script**

Create `tests/e2e_attach_smoke.sh`:

```bash
#!/bin/bash
# End-to-end: launch the GUI, attach with the CLI, assert, quit, assert cleanup.
set -u
APP=/tmp/ot-build-ht/OpenTerminal.app
CLI=/tmp/ot-build-ht/openterminalcli
ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
fail() { echo "FAIL: $1"; exit 1; }

rm -f "$ROOT/bridge.json"
open "$APP"; sleep 7
test -f "$ROOT/bridge.json" || fail "bridge.json not written"

"$CLI" status | grep -q "attached" || fail "status not attached"
"$CLI" mcp list | grep -q '"name"' || fail "mcp list empty"
"$CLI" --json mcp call get_quote '{"symbol":"AAPL"}' | grep -q '"success"' || fail "quote call failed"
"$CLI" status >/dev/null; [ $? -eq 0 ] || fail "status exit nonzero"

osascript -e 'quit app "OpenTerminal"' 2>/dev/null; sleep 3
test -f "$ROOT/bridge.json" && fail "bridge.json not removed on quit"

# Negative: with no app, status exits 3.
"$CLI" status; [ $? -eq 3 ] || fail "status should exit 3 when not running"
echo "PASS: e2e attach smoke"
```

- [ ] **Step 2: Run it**

```bash
chmod +x tests/e2e_attach_smoke.sh
bash tests/e2e_attach_smoke.sh
```
Expected: `PASS: e2e attach smoke`.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e_attach_smoke.sh
git commit -m "test(cli): end-to-end attach smoke (GUI <-> openterminalcli)"
```

---

## Task 10: Usage doc + final verification

**Files:**
- Create: `src/cli/README.md`

- [ ] **Step 1: Write the README**

Create `src/cli/README.md`:

```markdown
# openterminalcli (Phase 1 — attach mode)

Headless client that attaches to a **running** OpenTerminal GUI over its
localhost bridge. Read-only / non-destructive.

## Usage
    openterminalcli [--json] [--profile <name>] <group> <command> [args]

    status                          # is an instance attached?
    version
    mcp list | describe <tool> | call <tool> '<json>'
    hub topics | peek <topic> | request <topic>
    quote <SYM...>

## Exit codes
    0 ok · 2 usage · 3 no running instance · 4 token rejected
    5 tool returned success:false · 6 transport/HTTP error

## Notes
- Requires the GUI running with `bridge.autostart` on (default).
- stdout = data; stderr = diagnostics. Use `--json … | jq`.
- The CLI never sends the destructive token; live/destructive actions are
  out of scope for Phase 1 (see docs/design/2026-06-14-openterminalcli-design.md).
```

- [ ] **Step 2: Full regression — all unit tests + a clean release build of both targets**

```bash
ctest --test-dir /tmp/ot-build-test --output-on-failure        # all tst_* green
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli
/tmp/ot-build-ht/OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-bridge-discovery; echo "exit=$?"
```
Expected: all tests pass; both targets link; selftest `exit=0`.

- [ ] **Step 3: Commit**

```bash
git add src/cli/README.md
git commit -m "docs(cli): openterminalcli usage README"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** §"Bridge changes 1a/1b" → Tasks 2,3. §"CLI binary (BridgeDiscovery/BridgeClient/CommandDispatch)" → Tasks 4,5,6. §"Command tree" → Tasks 4,5,7,8 (`selftest` subcommand is explicitly Phase-2 per spec §3a and is NOT implemented here — only `--selftest-bridge-discovery` in the GUI binary, which is a build-time check, not a CLI command). §"Output & error contract / exit codes" → Task 7 `emit()` + Task 5 `status`. §"Profile resolution" → `profile_root_for` (Task 1) + `--profile` (Task 4). §"Claude-operability / no destructive token" → `BridgeClient` never sets `X-MCP-Allow-Destructive` (Task 6). §"Testing strategy" → Task 9.

**Known scope note:** the spec's `selftest <name>` CLI command is deferred to Phase 2 (it needs the headless core); Phase 1 ships `status/mcp/hub/quote/version`. This is stated in the spec (§3a) and here — not a gap.

**Type consistency:** `BridgeInfo`, `GlobalOpts`, `ClientResult`, `ClientStatus`, `DiscoveryError`, `resolve()`, `make_client()`, `emit()`, `get_tools()`, `call_tool()` are used identically across tasks. Tool names (`get_quote`, `datahub_list_topics`, `datahub_peek`, `datahub_request`) and arg keys (`symbol`, `topic`) match the verified codebase values.

**Placeholder scan:** no TBD/"handle errors"/"similar to" — every code step is complete.
