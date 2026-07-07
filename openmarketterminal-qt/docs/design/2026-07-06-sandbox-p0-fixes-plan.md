# Strategy Sandbox P0 — Automation Audit Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the five audit-blocking defects in the `automation` command group (dedup, log rotation + tail-read, profile correctness, `stop` disarms, spot horizon/edge semantics) so the Strategy Sandbox (separate plan) can build on sound plumbing.

**Architecture:** Extract the file-state helpers that are currently `static` inside `src/cli/CommandDispatch.cpp` into a new testable module `src/cli/automation/AutomationState.{h,cpp}`, make every path profile-aware, then fix behavior with TDD at two levels: unit tests on the module and end-to-end tests through `dispatch()` with a temp `$HOME` (the established pattern in `tests/tst_command_dispatch.cpp`).

**Tech Stack:** Qt 6 / C++17, QtTest + ctest, CMake (Ninja). Build dir: `openmarketterminal-qt/build` (already configured). Test build: same dir, targets `tst_*`.

## Global Constraints

- Spec: `docs/design/2026-07-06-strategy-sandbox-design.md` §3 (P0 items 1–5). This plan is P0 ONLY — no sandbox tables, no Paper Executor.
- Surgical edits: do not refactor unrelated code in `CommandDispatch.cpp` (22k lines) or `ServeCommand.cpp`.
- Every fix ships with a test that fails without it (neuter-verify: re-break the code, confirm the test goes red, restore).
- No new dependencies. No behavior change to the live gate chain (`SettingsGate`, auth checkers) — those are out of scope and verified good.
- Tests must not hit the network. Use temp `$HOME` isolation: `QTemporaryDir home; qputenv("HOME", home.path().toUtf8());` (pattern from `tests/tst_command_dispatch.cpp:110`).
- All work on branch `fix/automation-p0`.
- JSON output keys consumed by the GUI (`ProfileScreen.cpp`) may gain keys but never lose or rename existing ones.

---

### Task 0: Branch and baseline commit

The five fixes modify code that is currently **uncommitted** in the working tree (the `automation` command group diff). It must be committed as a baseline first so every fix is a reviewable delta.

**Files:**
- Modify (commit as-is, no edits): `src/cli/CommandDispatch.cpp`, `src/cli/ServeCommand.cpp`, `src/screens/crypto_trading/CryptoOrderEntry.cpp`, `src/screens/profile/ProfileScreen.cpp`, `src/screens/profile/ProfileScreen.h`

- [ ] **Step 1: Create branch and commit the working tree**

```bash
cd /Users/haydarevich/src/Open-Terminal
git checkout -b fix/automation-p0
git add openmarketterminal-qt/src/cli/CommandDispatch.cpp \
        openmarketterminal-qt/src/cli/ServeCommand.cpp \
        openmarketterminal-qt/src/screens/crypto_trading/CryptoOrderEntry.cpp \
        openmarketterminal-qt/src/screens/profile/ProfileScreen.cpp \
        openmarketterminal-qt/src/screens/profile/ProfileScreen.h
git commit -m "Add automation command group (pre-fix baseline)

Codex-authored automation arm/execute-next/24-7 + GUI panels, committed
as-is so the P0 audit fixes land as reviewable deltas on top."
```

- [ ] **Step 2: Verify clean tree and build**

Run: `git status --short` → empty (ignoring untracked). Then:
`cmake --build openmarketterminal-qt/build --target openterminalcli 2>&1 | tail -3`
Expected: `Linking CXX executable openterminalcli` with exit 0.

---

### Task 1: Extract `AutomationState` module (no behavior change)

Move the automation file-state helpers out of `CommandDispatch.cpp` into a compilable, testable module. Signatures already take a `profile` argument (wired to real profile resolution in Task 2 — in this task, pass through to the existing default-profile behavior so nothing changes yet).

**Files:**
- Create: `src/cli/automation/AutomationState.h`
- Create: `src/cli/automation/AutomationState.cpp`
- Modify: `src/cli/CommandDispatch.cpp` (delete the static helpers `automation_state_dir`, `automation_live_guard_path`, `automation_decisions_path`, `automation_orders_path`, `automation_read_json_object`, `automation_write_json_object`, `automation_append_jsonl`, `automation_latest_candidate`, `automation_submitted_today_count`; call the module instead)
- Modify: `CMakeLists.txt` (add the two new files to the `openterminalcli` target's source list, next to the other `src/cli/*.cpp` entries)
- Modify: `tests/CMakeLists.txt` (add `AutomationState.cpp` to `tst_command_dispatch` sources; add new target `tst_automation_state`)
- Test: `tests/tst_automation_state.cpp`

**Interfaces:**
- Produces (all in `namespace openmarketterminal::cli::automation`, used by Tasks 2–7):

```cpp
QString state_dir(const QString& profile);        // <profile root>/daemon (mkpath'd)
QString live_guard_path(const QString& profile);  // .../automation_live_guard.json
QString decisions_path(const QString& profile);   // .../scalp_decisions.jsonl
QString orders_path(const QString& profile);      // .../automation_orders.jsonl
QString consumed_path(const QString& profile);    // .../automation_consumed.json
QJsonObject read_json_object(const QString& path);
bool write_json_object(const QString& path, const QJsonObject& o, QString* error = nullptr);
bool append_jsonl(const QString& path, const QJsonObject& o, QString* error = nullptr);
QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error = nullptr);
int submitted_today_count(const QString& profile);
```

- [ ] **Step 1: Write the failing test**

`tests/tst_automation_state.cpp`:

```cpp
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "cli/automation/AutomationState.h"

using namespace openmarketterminal::cli::automation;

class TstAutomationState : public QObject {
    Q_OBJECT
  private slots:
    void guard_roundtrip() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QJsonObject guard{{"enabled", true}, {"max_order_usd", 100.0}};
        QString err;
        QVERIFY(write_json_object(live_guard_path(QStringLiteral("default")), guard, &err));
        const QJsonObject back = read_json_object(live_guard_path(QStringLiteral("default")));
        QCOMPARE(back.value("enabled").toBool(), true);
        QCOMPARE(back.value("max_order_usd").toDouble(), 100.0);
    }
    void latest_candidate_reads_fixture() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject good{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(now_ms)}};
        const QJsonObject stale{{"symbol", "BTC-USD"},
                                {"verdict", "PAPER TRADE CANDIDATE"},
                                {"action", "PAPER_LIMIT_BUY_ONLY"},
                                {"reference_price", 59000.0},
                                {"ts_ms", QString::number(now_ms - 3600 * 1000)}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), stale, &err));
        QVERIFY(append_jsonl(decisions_path("default"), good, &err));
        const QJsonObject c = latest_candidate("default", "BTC-USD", 15, &err);
        QVERIFY(!c.isEmpty());
        QCOMPARE(c.value("reference_price").toDouble(), 60000.0);
        // symbol filter must exclude
        QVERIFY(latest_candidate("default", "ETH-USD", 15, &err).isEmpty());
    }
    void submitted_today_counts_only_submitted() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString err;
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"ts", ts}, {"submitted", true}}, &err));
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"ts", ts}, {"dry_run", true}}, &err));
        QCOMPARE(submitted_today_count("default"), 1);
    }
};
QTEST_GUILESS_MAIN(TstAutomationState)
#include "tst_automation_state.moc"
```

Append to `tests/CMakeLists.txt` (follow the `tst_bridge_client` pattern at lines 27-31):

```cmake
add_executable(tst_automation_state
    tst_automation_state.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_automation_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_automation_state PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_automation_state COMMAND tst_automation_state)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build openmarketterminal-qt/build --target tst_automation_state`
Expected: FAIL — `cli/automation/AutomationState.h: No such file or directory`.

- [ ] **Step 3: Write the module**

`src/cli/automation/AutomationState.h`:

```cpp
#pragma once
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::cli::automation {

// All state lives under <profile root>/daemon. Task 1: profile is accepted but
// resolution still matches the daemon's profile_root_for(); see AutomationState.cpp.
QString state_dir(const QString& profile);
QString live_guard_path(const QString& profile);
QString decisions_path(const QString& profile);
QString orders_path(const QString& profile);
QString consumed_path(const QString& profile);

QJsonObject read_json_object(const QString& path);
bool write_json_object(const QString& path, const QJsonObject& o, QString* error = nullptr);
bool append_jsonl(const QString& path, const QJsonObject& o, QString* error = nullptr);

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error = nullptr);
int submitted_today_count(const QString& profile);

}  // namespace openmarketterminal::cli::automation
```

`src/cli/automation/AutomationState.cpp` — the bodies are the existing statics from
`CommandDispatch.cpp`, with paths rooted via `profile_root_for()` (the same function
the daemon uses, from `cli/BridgeDiscoveryFile.h`) instead of `ProfilePaths`:

```cpp
#include "cli/automation/AutomationState.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include "cli/BridgeDiscoveryFile.h"

namespace openmarketterminal::cli::automation {

QString state_dir(const QString& profile) {
    const QString dir = profile_root_for(profile) + QStringLiteral("/daemon");
    QDir().mkpath(dir);
    return dir;
}

QString live_guard_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_live_guard.json");
}
QString decisions_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/scalp_decisions.jsonl");
}
QString orders_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_orders.jsonl");
}
QString consumed_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_consumed.json");
}

QJsonObject read_json_object(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

bool write_json_object(const QString& path, const QJsonObject& o, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("could not commit %1").arg(path);
        return false;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

bool append_jsonl(const QString& path, const QJsonObject& o, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not append %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.write("\n");
    return true;
}

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error) {
    QFile f(decisions_path(profile));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("no paper decisions yet; start automation and daemon first");
        return {};
    }
    const QList<QByteArray> lines = f.readAll().split('\n');  // tail-read lands in Task 3
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString filter = symbol_filter.trimmed().toUpper();
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject d = doc.object();
        const QString symbol = d.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
        if (!filter.isEmpty() && symbol != filter)
            continue;
        if (d.value(QStringLiteral("verdict")).toString() != QLatin1String("PAPER TRADE CANDIDATE"))
            continue;
        if (d.value(QStringLiteral("action")).toString() != QLatin1String("PAPER_LIMIT_BUY_ONLY"))
            continue;
        bool ok = false;
        const qint64 ts_ms = d.value(QStringLiteral("ts_ms")).toString().toLongLong(&ok);
        if (!ok || ts_ms <= 0 || now_ms - ts_ms > static_cast<qint64>(max_age_sec) * 1000)
            continue;
        return d;
    }
    if (error) *error = QStringLiteral("no fresh approved paper candidate found");
    return {};
}

int submitted_today_count(const QString& profile) {
    QFile f(orders_path(profile));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;
    const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
    int count = 0;
    for (const QByteArray& raw : f.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("submitted")).toBool())
            continue;
        if (o.value(QStringLiteral("ts")).toString().startsWith(today))
            ++count;
    }
    return count;
}

}  // namespace openmarketterminal::cli::automation
```

Note: `profile_root_for()` lives in `src/cli/BridgeDiscoveryFile.cpp:29-33` and treats
empty/"default" as the app root, `X` as `<root>/profiles/X` — identical to the daemon's
path logic, which is exactly the point. Check the exact declared name/namespace in
`src/cli/BridgeDiscoveryFile.h` and adjust the `#include`/qualification to match.

- [ ] **Step 4: Rewire `CommandDispatch.cpp` and run tests**

In `CommandDispatch.cpp`: delete the nine static helpers listed above; add
`#include "cli/automation/AutomationState.h"`; at each former call site call the
namespaced function with `opts.profile` where a `GlobalOpts` is in scope. Two call
sites are inside functions that only receive profile-less state today
(`automation_latest_candidate` callers already have `opts`). Where a function had no
profile in scope, pass `QStringLiteral("default")` for now with a `// Task 2` marker
comment — Task 2 removes every such marker.

Add both files to the `openterminalcli` sources in the root `CMakeLists.txt`
(search for `src/cli/CommandDispatch.cpp` and add the new .cpp beside it), and add
`${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp` to the
`tst_command_dispatch` source list in `tests/CMakeLists.txt:11-18`.

Run:
```bash
cmake --build openmarketterminal-qt/build --target tst_automation_state openterminalcli tst_command_dispatch
cd openmarketterminal-qt/build && ctest -R "tst_automation_state|tst_command_dispatch" --output-on-failure
```
Expected: both suites PASS.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli/automation openmarketterminal-qt/src/cli/CommandDispatch.cpp \
        openmarketterminal-qt/CMakeLists.txt openmarketterminal-qt/tests
git commit -m "Extract automation state helpers into testable module"
```

---

### Task 2: Profile-correct automation state (audit fix #3)

Every automation state file must land under the profile named by `--profile`. Today it always lands in the default profile (proven: guard file recording `"profile":"codex-arm-test"` found in the default daemon dir).

**Files:**
- Modify: `src/cli/CommandDispatch.cpp` (`automation_arm_live_command`, `automation_disarm_live_command`, `automation_live_status_command`, `automation_execute_next_command` — every `// Task 2` marker from Task 1 becomes `opts.profile`)
- Modify: `src/cli/ServeCommand.cpp` (`daemon_automation_stop_command` and any daemon-side guard access use `automation::` paths with the `profile` parameter they already receive)
- Test: `tests/tst_automation_state.cpp` (new case), `tests/tst_command_dispatch.cpp` (new case)

**Interfaces:**
- Consumes: Task 1 module functions.
- Produces: guarantee "state path == `profile_root_for(opts.profile)/daemon/...` for ALL automation subcommands" relied on by every later task.

- [ ] **Step 1: Write the failing tests**

Add to `tests/tst_automation_state.cpp`:

```cpp
    void profile_isolation() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(write_json_object(live_guard_path("botlab"), QJsonObject{{"enabled", true}}, nullptr));
        // default profile must NOT see botlab's guard
        QVERIFY(read_json_object(live_guard_path("default")).isEmpty());
        QVERIFY(live_guard_path("botlab").contains(QStringLiteral("/profiles/botlab/daemon/")));
    }
```

Add to `tests/tst_command_dispatch.cpp` (uses the file's existing `capture_stdout` helper and `dispatch()`):

```cpp
    void automation_arm_respects_profile() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        int rc = -1;
        capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("--profile"), QStringLiteral("botlab"),
                           QStringLiteral("automation"), QStringLiteral("arm-bot"),
                           QStringLiteral("--max-order-usd"), QStringLiteral("50"),
                           QStringLiteral("--symbols"), QStringLiteral("BTC-USD"),
                           QStringLiteral("--yes"), QStringLiteral("--i-understand-live-risk")});
            return rc;
        });
        QCOMPARE(rc, 0);
        const QString botlab_guard = home.path() +
            QStringLiteral("/Library/Application Support/org.openterminal.OpenTerminal/profiles/botlab/daemon/automation_live_guard.json");
        const QString default_guard = home.path() +
            QStringLiteral("/Library/Application Support/org.openterminal.OpenTerminal/daemon/automation_live_guard.json");
        QVERIFY2(QFile::exists(botlab_guard), "guard must land in the armed profile");
        QVERIFY2(!QFile::exists(default_guard), "guard must NOT leak into the default profile");
    }
```

(If the platform path differs on Linux CI, derive the expected prefix by calling
`automation::live_guard_path("botlab")` in the test instead of hardcoding — preferred.)

- [ ] **Step 2: Run tests to verify failure**

Run: `ctest -R "tst_automation_state|tst_command_dispatch" --output-on-failure`
Expected: `profile_isolation` PASSES already (module is profile-correct from Task 1);
`automation_arm_respects_profile` FAILS — the guard lands in the default dir because
`arm-bot`'s call site still passes the Task 1 `"default"` marker.

- [ ] **Step 3: Fix all call sites**

In `CommandDispatch.cpp` replace every `// Task 2` marker with `opts.profile`. In
`ServeCommand.cpp`, wherever daemon code touches guard/orders/consumed files, use the
`automation::` path functions with the function's `profile` parameter. Verify with:
`grep -n "Task 2" src/cli/*.cpp` → no matches.

- [ ] **Step 4: Run tests to verify pass**

Run: `ctest -R "tst_automation_state|tst_command_dispatch" --output-on-failure`
Expected: PASS. Neuter-verify: temporarily revert one call site to `"default"`,
rerun, confirm RED, restore.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli openmarketterminal-qt/tests
git commit -m "Route all automation state through the requested profile"
```

---

### Task 3: Tail-read for decisions and orders scans (audit fix #2a)

`latest_candidate` and `submitted_today_count` currently `readAll()` files that reach hundreds of MB (762 MB observed; 3.3 s / 1.7 GB peak per run). Read only the tail.

**Files:**
- Modify: `src/cli/automation/AutomationState.h` / `.cpp`
- Test: `tests/tst_automation_state.cpp`

**Interfaces:**
- Produces: `QByteArray read_tail(const QString& path, qint64 max_bytes)` — returns the last `max_bytes` of the file starting at the first complete line (drops a leading partial line when truncated). Constant: `inline constexpr qint64 kTailBytes = 512 * 1024;` in the header. Both scan functions use it.

- [ ] **Step 1: Write the failing test**

```cpp
    void tail_read_finds_recent_candidate_in_large_file() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        // ~2 MB of filler no-trade rows, then one fresh candidate at the end.
        QString err;
        const QJsonObject filler{{"symbol", "BTC-USD"}, {"verdict", "NO TRADE"},
                                 {"action", "NO_ORDER"},
                                 {"pad", QString(400, QChar('x'))}};
        for (int i = 0; i < 4000; ++i)
            QVERIFY(append_jsonl(decisions_path("default"), filler, &err));
        const QJsonObject good{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(QDateTime::currentMSecsSinceEpoch())}};
        QVERIFY(append_jsonl(decisions_path("default"), good, &err));
        QElapsedTimer t; t.start();
        const QJsonObject c = latest_candidate("default", "BTC-USD", 15, &err);
        QVERIFY(!c.isEmpty());
        QCOMPARE(c.value("reference_price").toDouble(), 60000.0);
        // Behavioral proof of tail-read: a partial first line must not break parsing.
        const QByteArray tail = read_tail(decisions_path("default"), 1024);
        QVERIFY(!tail.startsWith('{') || tail.startsWith("{\""));  // starts at a line boundary
        QVERIFY(tail.endsWith("\n"));
        Q_UNUSED(t);
    }
    void read_tail_small_file_returns_all() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QString err;
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"a", 1}}, &err));
        const QByteArray whole = read_tail(orders_path("default"), 512 * 1024);
        QCOMPARE(whole.count('\n'), 1);
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build openmarketterminal-qt/build --target tst_automation_state && ctest -R tst_automation_state --output-on-failure`
Expected: FAIL — `read_tail` undeclared.

- [ ] **Step 3: Implement `read_tail` and switch both scanners**

Header additions:

```cpp
inline constexpr qint64 kTailBytes = 512 * 1024;
QByteArray read_tail(const QString& path, qint64 max_bytes = kTailBytes);
```

Implementation:

```cpp
QByteArray read_tail(const QString& path, qint64 max_bytes) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = f.size();
    if (size > max_bytes) {
        f.seek(size - max_bytes);
        QByteArray data = f.readAll();
        const int nl = data.indexOf('\n');
        return nl >= 0 ? data.mid(nl + 1) : QByteArray{};  // drop leading partial line
    }
    return f.readAll();
}
```

In `latest_candidate` and `submitted_today_count`, replace
`f.readAll()` (and the open) with `read_tail(<path>, kTailBytes)`; keep the
"file missing → error/0" behavior by checking `QFile::exists` first for
`latest_candidate`'s error message.

Documented consequence (add as comment): candidates older than the last 512 KB of
the file are invisible — acceptable because candidates expire in ≤ 3600 s anyway and
Task 5 caps file size.

- [ ] **Step 4: Run tests to verify pass**

Run: `ctest -R tst_automation_state --output-on-failure` → PASS.
Neuter-verify by making `read_tail` return `f.readAll()` unconditionally minus the
boundary trim — the boundary assertions go RED — then restore.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli/automation openmarketterminal-qt/tests/tst_automation_state.cpp
git commit -m "Tail-read automation jsonl scans instead of loading whole files"
```

---

### Task 4: Candidate dedup — consumed set (audit fix #1)

`execute-next` must never act twice on the same candidate. Key: scalp candidates → `"<SYMBOL>|<ts_ms>"` (the engine journals a symbol at most once per second, so the pair is unique); spot candidates → the journal row's UUID `id`. Consumption is recorded when a live submission is **attempted** (not on dry-run), before the broker call, so a retry storm on one candidate is impossible even across crashes.

**Files:**
- Modify: `src/cli/automation/AutomationState.h` / `.cpp`
- Modify: `src/cli/CommandDispatch.cpp` (`automation_execute_next_command`)
- Test: `tests/tst_automation_state.cpp`, `tests/tst_command_dispatch.cpp`

**Interfaces:**
- Produces:

```cpp
QString candidate_key(const QJsonObject& decision);   // spot rows: value of "id"; else "<SYMBOL>|<ts_ms>"
bool is_consumed(const QString& profile, const QString& key);
bool mark_consumed(const QString& profile, const QString& key, QString* error = nullptr);
```

`consumed_path(profile)` file format: `{"keys": {"<key>": "<iso ts>"}}`; `mark_consumed` prunes entries older than 48 h on write. `latest_candidate` gains a final filter: skip rows whose key `is_consumed`.

- [ ] **Step 1: Write the failing tests**

`tests/tst_automation_state.cpp`:

```cpp
    void consumed_candidate_is_skipped() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QJsonObject cand{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", ts}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), cand, &err));
        QCOMPARE(candidate_key(cand), QStringLiteral("BTC-USD|") + ts);
        QVERIFY(!latest_candidate("default", "BTC-USD", 60, &err).isEmpty());
        QVERIFY(mark_consumed("default", candidate_key(cand), &err));
        QVERIFY(is_consumed("default", candidate_key(cand)));
        QVERIFY2(latest_candidate("default", "BTC-USD", 60, &err).isEmpty(),
                 "consumed candidate must not be returned again");
    }
    void spot_key_prefers_journal_id() {
        const QJsonObject spot{{"id", "abc-123"}, {"symbol", "BTC-USD"}, {"ts_ms", "1"}};
        QCOMPARE(candidate_key(spot), QStringLiteral("abc-123"));
    }
```

`tests/tst_command_dispatch.cpp` — dry-run must not consume:

```cpp
    void automation_dry_run_does_not_consume() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        const QJsonObject cand{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(QDateTime::currentMSecsSinceEpoch())}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), cand, &err));
        int rc = -1;
        const QString out = capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("automation"),
                           QStringLiteral("execute-next"), QStringLiteral("--symbol"),
                           QStringLiteral("BTC-USD"), QStringLiteral("--dry-run")});
            return rc;
        });
        QCOMPARE(rc, 0);
        QVERIFY(out.contains(QStringLiteral("\"dry_run\":true")));
        QVERIFY2(!QFile::exists(consumed_path("default")), "dry-run must not consume");
        // second dry-run still finds the candidate
        const QString out2 = capture_stdout([&]() {
            return dispatch({QStringLiteral("--json"), QStringLiteral("automation"),
                             QStringLiteral("execute-next"), QStringLiteral("--symbol"),
                             QStringLiteral("BTC-USD"), QStringLiteral("--dry-run")});
        });
        QVERIFY(out2.contains(QStringLiteral("\"order\"")));
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `ctest -R "tst_automation_state|tst_command_dispatch" --output-on-failure`
Expected: FAIL — `candidate_key`/`is_consumed`/`mark_consumed` undeclared.

- [ ] **Step 3: Implement**

`AutomationState.cpp`:

```cpp
QString candidate_key(const QJsonObject& decision) {
    const QString id = decision.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        return id;
    return decision.value(QStringLiteral("symbol")).toString().trimmed().toUpper() +
           QLatin1Char('|') + decision.value(QStringLiteral("ts_ms")).toString();
}

bool is_consumed(const QString& profile, const QString& key) {
    return read_json_object(consumed_path(profile))
        .value(QStringLiteral("keys")).toObject().contains(key);
}

bool mark_consumed(const QString& profile, const QString& key, QString* error) {
    QJsonObject doc = read_json_object(consumed_path(profile));
    QJsonObject keys = doc.value(QStringLiteral("keys")).toObject();
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-48 * 3600);
    QJsonObject pruned;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const QDateTime ts = QDateTime::fromString(it.value().toString(), Qt::ISODateWithMs);
        if (ts.isValid() && ts >= cutoff)
            pruned.insert(it.key(), it.value());
    }
    pruned.insert(key, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    doc[QStringLiteral("keys")] = pruned;
    return write_json_object(consumed_path(profile), doc, error);
}
```

In `latest_candidate`, after the freshness check and before `return d;`:

```cpp
        if (is_consumed(profile, candidate_key(d)))
            continue;
```

In `automation_execute_next_command` (`CommandDispatch.cpp`), the live branch (after
gate checks pass, immediately BEFORE `call_headless_tool_json`):

```cpp
    QString consume_error;
    if (!automation::mark_consumed(opts.profile, automation::candidate_key(decision), &consume_error)) {
        return automation_emit_object(opts, QJsonObject{{"submitted", false},
                                                        {"reason", QStringLiteral("could not record candidate consumption: %1").arg(consume_error)}});
    }
```

(Fail-closed: if we cannot record consumption we do not trade.) The spot lane also
skips consumed rows: in `automation_latest_spot_candidate`'s row loop, after the
edge/confidence checks, `if (automation::is_consumed(opts.profile, q.value(0).toString())) continue;`.

- [ ] **Step 4: Run tests to verify pass**

Run: `ctest -R "tst_automation_state|tst_command_dispatch" --output-on-failure` → PASS.
Neuter-verify: comment out the `is_consumed` filter in `latest_candidate`, confirm
`consumed_candidate_is_skipped` goes RED, restore.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli openmarketterminal-qt/tests
git commit -m "Dedup automation candidates via persisted consumed set"
```

---

### Task 5: Jsonl rotation in the daemon (audit fix #2b)

The daemon's `append_jsonl` (ServeCommand.cpp:299-306) grows `scalp_decisions.jsonl` / `scalp_ticks.jsonl` without bound (762 MB / 330 MB observed). Rotate at a byte cap, keeping one previous generation.

**Files:**
- Modify: `src/cli/automation/AutomationState.h` / `.cpp` (add `append_jsonl_rotating`)
- Modify: `src/cli/ServeCommand.cpp` (scalp decisions + ticks appends use the rotating variant)
- Test: `tests/tst_automation_state.cpp`

**Interfaces:**
- Produces: `bool append_jsonl_rotating(const QString& path, const QJsonObject& o, qint64 max_bytes = kRotateBytes, QString* error = nullptr);` with `inline constexpr qint64 kRotateBytes = 64LL * 1024 * 1024;`. Rotation: when the file exceeds `max_bytes`, remove `path + ".1"` if present, rename current → `path + ".1"`, start fresh.

- [ ] **Step 1: Write the failing test**

```cpp
    void rotation_caps_file_and_keeps_one_generation() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QString path = decisions_path("default");
        const QJsonObject row{{"pad", QString(100, QChar('x'))}};
        QString err;
        for (int i = 0; i < 50; ++i)
            QVERIFY(append_jsonl_rotating(path, row, 2048, &err));  // tiny cap for the test
        QVERIFY(QFile::exists(path));
        QVERIFY(QFile::exists(path + ".1"));
        QVERIFY2(QFileInfo(path).size() <= 2048 + 256, "active file must stay near the cap");
        // every line in the active file is complete JSON
        const QByteArray data = read_tail(path, 1 << 20);
        for (const QByteArray& line : data.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            QJsonParseError pe;
            QJsonDocument::fromJson(line, &pe);
            QCOMPARE(pe.error, QJsonParseError::NoError);
        }
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `ctest -R tst_automation_state --output-on-failure`
Expected: FAIL — `append_jsonl_rotating` undeclared.

- [ ] **Step 3: Implement and switch daemon call sites**

```cpp
bool append_jsonl_rotating(const QString& path, const QJsonObject& o,
                           qint64 max_bytes, QString* error) {
    if (QFileInfo::exists(path) && QFileInfo(path).size() >= max_bytes) {
        QFile::remove(path + QStringLiteral(".1"));
        if (!QFile::rename(path, path + QStringLiteral(".1"))) {
            if (error) *error = QStringLiteral("could not rotate %1").arg(path);
            return false;
        }
    }
    return append_jsonl(path, o, error);
}
```

In `ServeCommand.cpp`, the `DaemonScalpEngine` decision and tick appends switch from
the local `append_jsonl` to `automation::append_jsonl_rotating` (default cap). Add
the include and, in `tests/CMakeLists.txt`, `AutomationState.cpp` is already compiled
into `tst_command_dispatch` (which also compiles ServeCommand.cpp) from Task 1 —
verify the target still links.

- [ ] **Step 4: Run tests + build daemon target**

Run: `ctest -R tst_automation_state --output-on-failure && cmake --build openmarketterminal-qt/build --target openterminalcli`
Expected: PASS + clean link.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli openmarketterminal-qt/tests/tst_automation_state.cpp
git commit -m "Rotate daemon scalp jsonl files at 64MB"
```

---

### Task 6: `automation stop` disarms; status surfaces the guard (audit fix #4)

`automation stop` currently disables the scanner and 24/7 jobs but silently leaves the live guard armed until expiry, and `automation status` never mentions it.

**Files:**
- Modify: `src/cli/ServeCommand.cpp` (`daemon_automation_stop_command` disarms the guard; `daemon_scalp_command` status branch adds a `live_guard` key + text line)
- Test: `tests/tst_command_dispatch.cpp`

**Interfaces:**
- Consumes: `automation::live_guard_path/read_json_object/write_json_object` (profile-aware from Task 2).
- Produces: stop output gains `"live_guard_disarmed": bool`; status JSON gains `"live_guard": {object}` (additive keys only — GUI-safe).

- [ ] **Step 1: Write the failing test**

```cpp
    void automation_stop_disarms_live_guard() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        QVERIFY(write_json_object(live_guard_path("default"),
                                  QJsonObject{{"enabled", true},
                                              {"expires_at", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODateWithMs)}},
                                  nullptr));
        int rc = -1;
        const QString out = capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("automation"), QStringLiteral("stop")});
            return rc;
        });
        QCOMPARE(rc, 0);
        QVERIFY(out.contains(QStringLiteral("\"live_guard_disarmed\":true")));
        QCOMPARE(read_json_object(live_guard_path("default")).value("enabled").toBool(), false);
    }
    void automation_status_shows_guard() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        QVERIFY(write_json_object(live_guard_path("default"),
                                  QJsonObject{{"enabled", true}}, nullptr));
        const QString out = capture_stdout([&]() {
            return dispatch({QStringLiteral("--json"), QStringLiteral("automation"), QStringLiteral("status")});
        });
        QVERIFY2(out.contains(QStringLiteral("\"live_guard\"")), "status must surface the live guard");
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `ctest -R tst_command_dispatch --output-on-failure`
Expected: both new cases FAIL (no `live_guard_disarmed`, no `live_guard`).

- [ ] **Step 3: Implement**

In `daemon_automation_stop_command` (ServeCommand.cpp), before building the output
object:

```cpp
    QJsonObject guard = automation::read_json_object(automation::live_guard_path(profile));
    const bool was_armed = guard.value(QStringLiteral("enabled")).toBool();
    if (was_armed) {
        guard[QStringLiteral("enabled")] = false;
        guard[QStringLiteral("disarmed_at")] = now_utc();
        guard[QStringLiteral("disarmed_by")] = QStringLiteral("automation stop");
        QString guard_error;
        if (!automation::write_json_object(automation::live_guard_path(profile), guard, &guard_error)) {
            std::fprintf(stderr, "guard disarm failed: %s\n", qUtf8Printable(guard_error));
            return 7;
        }
    }
```

and add `{"live_guard_disarmed", was_armed}` plus `{"live_guard", guard}` to the
output object; in text mode print `live guard    disarmed` when `was_armed`.

In the `daemon scalp status` branch, add to the emitted JSON object:
`{"live_guard", automation::read_json_object(automation::live_guard_path(profile))}`
and in text mode one line:
`std::printf("live guard    %s\n", guard.value("enabled").toBool() ? "ARMED" : "off");`

- [ ] **Step 4: Run tests to verify pass**

Run: `ctest -R tst_command_dispatch --output-on-failure` → PASS. Neuter-verify the
stop path by skipping the disarm write, confirm RED, restore.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli/ServeCommand.cpp openmarketterminal-qt/tests/tst_command_dispatch.cpp
git commit -m "Disarm live guard on automation stop and surface it in status"
```

---

### Task 7: Spot candidate horizon filter + honest edge semantics (audit fix #5)

The spot query consumes any `edge crypto-recommend` row — including 15-second scalp-gate rows — and its `min_spot_edge_bps` gate compares a probability-edge fraction against a threshold meant as price bps. Fix: (a) pure row filter requiring horizon ≥ 60 s; (b) report the value honestly as `probability_edge_after_cost_bps` (keep the old key as an alias for one release; the GUI does not read it).

**Files:**
- Modify: `src/cli/automation/AutomationState.h` / `.cpp` (pure filter)
- Modify: `src/cli/CommandDispatch.cpp` (`automation_latest_spot_candidate` uses the filter; output keys)
- Test: `tests/tst_automation_state.cpp`

**Interfaces:**
- Produces:

```cpp
// Horizon strings look like "15s", "60s", "1h", "4h", "1d". Unparseable → 0 (rejected).
int horizon_seconds(const QString& horizon);
bool spot_row_passes(const QString& horizon, double edge_after_cost_fraction,
                     double confidence, double min_edge_fraction, double min_confidence);
// passes iff horizon_seconds >= 60 && edge >= min_edge && confidence >= min_confidence
```

- [ ] **Step 1: Write the failing test**

```cpp
    void spot_row_filter_rejects_short_horizons_and_weak_edges() {
        QCOMPARE(horizon_seconds(QStringLiteral("15s")), 15);
        QCOMPARE(horizon_seconds(QStringLiteral("60s")), 60);
        QCOMPARE(horizon_seconds(QStringLiteral("1h")), 3600);
        QCOMPARE(horizon_seconds(QStringLiteral("4h")), 14400);
        QCOMPARE(horizon_seconds(QStringLiteral("1d")), 86400);
        QCOMPARE(horizon_seconds(QStringLiteral("garbage")), 0);
        // 15s scalp-gate row must NEVER feed the spot lane, however good it looks
        QVERIFY(!spot_row_passes("15s", 0.20, 0.95, 0.005, 0.80));
        QVERIFY(spot_row_passes("60s", 0.20, 0.95, 0.005, 0.80));
        QVERIFY(!spot_row_passes("60s", 0.004, 0.95, 0.005, 0.80));  // edge below gate
        QVERIFY(!spot_row_passes("60s", 0.20, 0.79, 0.005, 0.80));   // confidence below gate
        QVERIFY(!spot_row_passes(QString(), 0.20, 0.95, 0.005, 0.80));
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `ctest -R tst_automation_state --output-on-failure`
Expected: FAIL — functions undeclared.

- [ ] **Step 3: Implement**

```cpp
int horizon_seconds(const QString& horizon) {
    const QString h = horizon.trimmed().toLower();
    if (h.isEmpty())
        return 0;
    bool ok = false;
    const double n = h.left(h.size() - 1).toDouble(&ok);
    if (!ok || n <= 0)
        return 0;
    switch (h.back().toLatin1()) {
        case 's': return static_cast<int>(n);
        case 'm': return static_cast<int>(n * 60);
        case 'h': return static_cast<int>(n * 3600);
        case 'd': return static_cast<int>(n * 86400);
        default: return 0;
    }
}

bool spot_row_passes(const QString& horizon, double edge_after_cost_fraction,
                     double confidence, double min_edge_fraction, double min_confidence) {
    if (horizon_seconds(horizon) < 60)
        return false;
    return edge_after_cost_fraction >= min_edge_fraction && confidence >= min_confidence;
}
```

In `automation_latest_spot_candidate` (CommandDispatch.cpp), replace the inline
edge/confidence check in the row loop with:

```cpp
        const double edge_fraction = q.value(10).toDouble();
        const double confidence = q.value(12).toDouble();
        if (!automation::spot_row_passes(q.value(4).toString(), edge_fraction, confidence,
                                         min_edge_after_cost_bps / 10000.0, min_confidence))
            continue;
```

In the returned candidate object, add
`{"probability_edge_after_cost_bps", edge_fraction * 10000.0}` and
`{"edge_semantics", "probability edge (model_prob - 0.5) after costs; NOT expected price return"}`,
keeping the existing `edge_after_cost_bps` key as a deprecated alias.

- [ ] **Step 4: Run tests to verify pass**

Run: `ctest -R tst_automation_state --output-on-failure` → PASS.
Full check: `ctest --output-on-failure` (all suites) and
`cmake --build openmarketterminal-qt/build --target openterminalcli OpenTerminal`.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli openmarketterminal-qt/tests
git commit -m "Filter spot candidates by horizon and label probability edge honestly"
```

---

### Task 8: Full regression gate

**Files:** none new.

- [ ] **Step 1: Full test suite**

```bash
cd openmarketterminal-qt/build && ctest --output-on-failure
```
Expected: ALL suites pass (previously 21+; now includes `tst_automation_state`).

- [ ] **Step 2: Both binaries link, GUI selftests pass**

```bash
cmake --build . --target openterminalcli OpenTerminal
for t in $(./OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-list 2>/dev/null || echo ""); do :; done
```
Run the 9 GUI `--selftest-*` one-shots per the repo's established gate (see
`.github/workflows/regression.yml` for the canonical list); every one must exit 0.

- [ ] **Step 3: Live-fire smoke on the real profile (read-only)**

```bash
./openterminalcli --json automation live-status | head -1
./openterminalcli --json automation execute-next --symbol BTC-USD --dry-run
```
Expected: sub-second completion (tail-read; was 3.3 s), `"submitted":false`, and no
guard/orders files created outside the requested profile.

- [ ] **Step 4: Commit any stragglers, push branch**

```bash
git status --short   # expect clean
git push -u origin fix/automation-p0
```

---

## Self-review notes

- **Spec coverage:** §3.1 dedup → Task 4; §3.2 rotation + tail-read → Tasks 3+5; §3.3 profile → Task 2; §3.4 stop/status → Task 6; §3.5 edge units + horizon → Task 7. Module extraction (Task 1) is the enabling refactor; Task 0 is baseline hygiene; Task 8 is the repo's regression discipline.
- **Deliberately out of scope (Plan 2 — sandbox core):** registry, Paper Executor, exits, resolver, scorer, leaderboard, eligibility; `jobs.json` locking (QLockFile) — it touches the daemon scheduler and is scheduled with Plan 2 where jobs are extended.
- **Type consistency:** all new symbols live in `openmarketterminal::cli::automation`; `latest_candidate`/`submitted_today_count` keep behavior-compatible signatures from Task 1 through Task 4 refinements.
- **Known judgment calls encoded above:** consumption on live *attempt* (fail-closed if unrecordable); tail window blindness documented and bounded by rotation; `.1` single-generation rotation (older data is the sandbox's job to archive, not the hot path's).
