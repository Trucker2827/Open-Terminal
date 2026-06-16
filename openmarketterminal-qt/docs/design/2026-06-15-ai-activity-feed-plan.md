# AI-Activity Feed — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Surface every AI/CLI trading action to the human in real time (toast on any screen) and in a persistent panel (Settings → AI Activity), driven off the single `trade_audit` chokepoint.

**Architecture:** `TradeAuditRepository::append()` publishes one `"trade.audit"` EventBus event (fire-and-forget). A main-thread `AiActivityNotifier` connects to the `EventBus::eventPublished` **signal** (queued — safe even though `append()` fires on the bridge worker), runs a **pure formatter** to decide toast vs. not, posts a `ToastService` toast for terminal/committed outcomes, and feeds every row to the `AiActivitySection` panel (loads `recent(200)`, prepends live).

**Tech Stack:** Qt6 C++20, EventBus (`src/core/events/EventBus.h`), `TradeAuditRepository`, `ToastService` (`src/ui/notifications/NotificationService.h`), `QTableWidget` (mirror `PolymarketActivityFeed`), QtTest.

**Builds:** test `/tmp/ot-build-test` (`-DOPENMARKETTERMINAL_BUILD_TESTS=ON`), GUI `/tmp/ot-build-ht`. Branch `feat/ai-activity-feed` (spec already committed there).

**Verification discipline:** tests must RUN and FAIL without the change (neuter-proof). QtTest `QVERIFY/QCOMPARE` only. Confirm the `.o` rebuilt after edits.

**Toast rule (LOCKED):** `toast = (phase != "prepare") AND decision(lowercased) ∈ {filled, partially_filled, accepted, submitted, new, open, cancelled, canceled, rejected, denied}`. Never toast `prepare` phase or validation-only decisions (`ok, draft, valid, prepared, ""`). Severity: filled/partially_filled/cancelled/canceled → Success; accepted/submitted/new/open → Info; rejected/denied → Error.

---

## File structure

| File | Responsibility | Change | Target |
|---|---|---|---|
| `src/storage/repositories/TradeAuditRepository.{h,cpp}` | audit insert + map helpers + emit | add `audit_row_to_map`/`audit_row_from_map`; emit `"trade.audit"` on append | core |
| `src/trading/ai_activity/AiActivityFormat.{h,cpp}` | **pure** toast/format logic | **create** `ActivityView` + `format_activity()` | core (testable) |
| `src/app/AiActivityNotifier.{h,cpp}` | main-thread bridge: signal→toast+feed | **create** | GUI |
| `src/screens/settings/AiActivitySection.{h,cpp}` | the feed panel (QTableWidget) | **create**, mirror `PolymarketActivityFeed` | GUI |
| `src/screens/settings/SettingsScreen.cpp` | register the tab | add section factory + nav label | GUI |
| `src/app/WindowFrame*` | own the notifier; connect to the section | wire at shell startup | GUI |
| `tests/tst_ai_activity.cpp` + `tests/CMakeLists.txt` | unit tests | **create** + register | test |

---

## Task 1: Emit `"trade.audit"` from `TradeAuditRepository::append` + map helpers

**Files:** Modify `src/storage/repositories/TradeAuditRepository.{h,cpp}`; Create `tests/tst_ai_activity.cpp`; Modify `tests/CMakeLists.txt`.

- [ ] **Step 1: Declare the map helpers** in `TradeAuditRepository.h` (free functions in `namespace openmarketterminal`, after the `TradeAuditRow` struct):
```cpp
#include <QVariantMap>
// ...
/// Lossless map view of an audit row (used by the "trade.audit" EventBus event).
QVariantMap audit_row_to_map(const TradeAuditRow& row);
TradeAuditRow audit_row_from_map(const QVariantMap& m);
```

- [ ] **Step 2: Write the failing test** — create `tests/tst_ai_activity.cpp`:
```cpp
#include "storage/repositories/TradeAuditRepository.h"
#include "core/events/EventBus.h"
#include "cli/HeadlessRuntime.h"   // copy the temp-DB bring-up pattern from tst_settings_gate.cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QVariantMap>

using namespace openmarketterminal;

class TestAiActivity : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    cli::HeadlessRuntime rt_;
  private slots:
    void initTestCase();      // open a temp DB (mirror tst_settings_gate.cpp)
    void cleanupTestCase();
    void appendPublishesTradeAuditEvent();
};

void TestAiActivity::initTestCase() {
    QVERIFY(home_.isValid());
    qputenv("HOME", home_.path().toUtf8());
    auto r = rt_.init("default");
    QVERIFY2(r.ok, qPrintable(r.error));
}
void TestAiActivity::cleanupTestCase() { rt_.shutdown(); }

void TestAiActivity::appendPublishesTradeAuditEvent() {
    QVariantMap got; int fires = 0;
    auto id = EventBus::instance().subscribe("trade.audit", [&](const QVariantMap& m){ got = m; ++fires; });
    TradeAuditRow row;
    row.ts = "2026-06-15T20:00:00Z"; row.phase = "fast"; row.tool = "fast_submit_order";
    row.account = "acct-1"; row.mode = "live"; row.decision = "filled";
    row.reason = "filled"; row.intent_json = R"({"symbol":"AAPL","side":"buy","quantity":1})";
    auto wr = TradeAuditRepository::instance().append(row);
    QVERIFY2(wr.is_ok(), "append failed");
    QCOMPARE(fires, 1);
    QCOMPARE(got.value("tool").toString(), QString("fast_submit_order"));
    QCOMPARE(got.value("decision").toString(), QString("filled"));
    QCOMPARE(got.value("mode").toString(), QString("live"));
    EventBus::instance().unsubscribe(id);
}

QTEST_MAIN(TestAiActivity)
#include "tst_ai_activity.moc"
```
(If `HeadlessRuntime` bring-up differs, copy the exact `initTestCase` from `tests/tst_settings_gate.cpp`.)

- [ ] **Step 3: Register in `tests/CMakeLists.txt`** (mirror `tst_fast_live`):
```cmake
add_executable(tst_ai_activity tst_ai_activity.cpp)
target_include_directories(tst_ai_activity PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ai_activity PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ai_activity COMMAND tst_ai_activity)
```

- [ ] **Step 4: Build + run — expect FAIL** (`fires == 0`, append doesn't publish yet):
```
cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON >/dev/null
cmake --build /tmp/ot-build-test --target tst_ai_activity -j8 && ctest --test-dir /tmp/ot-build-test -R tst_ai_activity --output-on-failure
```

- [ ] **Step 5: Implement** in `TradeAuditRepository.cpp`. Add `#include "core/events/EventBus.h"` and `#include <QVariantMap>`. Add the helpers + emit:
```cpp
QVariantMap audit_row_to_map(const TradeAuditRow& row) {
    return {{"ts", row.ts}, {"phase", row.phase}, {"tool", row.tool}, {"account", row.account},
            {"mode", row.mode}, {"intent_json", row.intent_json}, {"decision", row.decision},
            {"reason", row.reason}, {"risk_snapshot_json", row.risk_snapshot_json}};
}
TradeAuditRow audit_row_from_map(const QVariantMap& m) {
    TradeAuditRow r;
    r.ts = m.value("ts").toString(); r.phase = m.value("phase").toString();
    r.tool = m.value("tool").toString(); r.account = m.value("account").toString();
    r.mode = m.value("mode").toString(); r.intent_json = m.value("intent_json").toString();
    r.decision = m.value("decision").toString(); r.reason = m.value("reason").toString();
    r.risk_snapshot_json = m.value("risk_snapshot_json").toString();
    return r;
}
```
And change `append` to emit after a successful insert:
```cpp
Result<void> TradeAuditRepository::append(const TradeAuditRow& row) {
    auto r = exec_write("INSERT INTO trade_audit "
                        "(ts, phase, tool, account, mode, intent_json, decision, reason, risk_snapshot_json) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        {row.ts, row.phase, row.tool, row.account, row.mode, row.intent_json,
                         row.decision, row.reason, row.risk_snapshot_json});
    if (r.is_ok()) {
        // Fire-and-forget real-time signal for the AI-activity feed. The audit row
        // is already committed; a publish issue must never affect the write.
        EventBus::instance().publish(QStringLiteral("trade.audit"), audit_row_to_map(row));
    }
    return r;
}
```

- [ ] **Step 6: Build + run — expect PASS.** Neuter-proof: comment out the `EventBus::...publish(...)` line, rebuild, confirm `appendPublishesTradeAuditEvent` FAILS (`fires==0`), restore.

- [ ] **Step 7: Commit**
```bash
git add src/storage/repositories/TradeAuditRepository.h src/storage/repositories/TradeAuditRepository.cpp tests/tst_ai_activity.cpp tests/CMakeLists.txt
git commit -m "feat(activity): emit trade.audit EventBus event on audit append"
```

---

## Task 2: Pure formatter `AiActivityFormat`

**Files:** Create `src/trading/ai_activity/AiActivityFormat.{h,cpp}`; Modify `tests/tst_ai_activity.cpp`; Modify `CMakeLists.txt` (add the .cpp to `openterminal_core` sources).

- [ ] **Step 1: Create the header** `src/trading/ai_activity/AiActivityFormat.h`:
```cpp
#pragma once
#include "storage/repositories/TradeAuditRepository.h"
#include <QString>

namespace openmarketterminal::trading {

/// Presentation view derived from a trade_audit row (pure; no Qt-widget deps).
struct ActivityView {
    bool toast = false;
    enum class Severity { Info, Success, Warning, Error };
    Severity severity = Severity::Info;
    QString message;       // toast text, e.g. "AI · fast_submit_order · BUY 1 AAPL → filled (live)"
    // panel columns:
    QString time, tool, account_mode, action, decision, reason;
};

/// Pure: decide whether a row toasts, its severity, and the formatted strings.
ActivityView format_activity(const TradeAuditRow& row);

} // namespace openmarketterminal::trading
```

- [ ] **Step 2: Write failing tests** — add to `tests/tst_ai_activity.cpp`:
```cpp
#include "trading/ai_activity/AiActivityFormat.h"
// slots:
void formatToastsTerminalOutcomes();
void formatNoToastForPrepareOrValidation();
void formatSeverityMapping();
void formatMessageAndMalformedIntent();
```
```cpp
using openmarketterminal::trading::format_activity;
using Sev = openmarketterminal::trading::ActivityView::Severity;
static TradeAuditRow mk(const QString& phase, const QString& tool, const QString& decision,
                        const QString& mode = "live", const QString& intent = "{}") {
    TradeAuditRow r; r.ts="2026-06-15T20:00:00Z"; r.phase=phase; r.tool=tool;
    r.decision=decision; r.mode=mode; r.intent_json=intent; r.reason=decision; return r;
}

void TestAiActivity::formatToastsTerminalOutcomes() {
    for (const char* d : {"filled","partially_filled","accepted","submitted","new","open",
                          "cancelled","canceled","rejected","denied"})
        QVERIFY2(format_activity(mk("fast","fast_submit_order",d)).toast, d);
    // case-insensitive
    QVERIFY(format_activity(mk("submit","submit_order","FILLED")).toast);
}
void TestAiActivity::formatNoToastForPrepareOrValidation() {
    QVERIFY(!format_activity(mk("prepare","prepare_order","ok")).toast);     // prepare phase
    QVERIFY(!format_activity(mk("prepare","prepare_order","filled")).toast); // prepare never toasts
    for (const char* d : {"ok","draft","valid","prepared",""})
        QVERIFY2(!format_activity(mk("submit","submit_order",d)).toast, d);  // validation-only
}
void TestAiActivity::formatSeverityMapping() {
    QCOMPARE(format_activity(mk("fast","fast_submit_order","filled")).severity, Sev::Success);
    QCOMPARE(format_activity(mk("fast","cancel_order","cancelled")).severity, Sev::Success);
    QCOMPARE(format_activity(mk("fast","fast_submit_order","accepted")).severity, Sev::Info);
    QCOMPARE(format_activity(mk("submit","submit_order","rejected")).severity, Sev::Error);
    QCOMPARE(format_activity(mk("fast","fast_submit_order","denied")).severity, Sev::Error);
}
void TestAiActivity::formatMessageAndMalformedIntent() {
    auto v = format_activity(mk("fast","fast_submit_order","filled","live",
                                R"({"symbol":"AAPL","side":"buy","quantity":1})"));
    QVERIFY(v.message.contains("fast_submit_order"));
    QVERIFY(v.message.contains("AAPL"));
    QVERIFY(v.message.contains("filled"));
    QVERIFY(v.message.contains("live"));
    // malformed intent → no crash, falls back to tool name
    auto v2 = format_activity(mk("fast","fast_submit_order","filled","live","not json"));
    QVERIFY(v2.message.contains("fast_submit_order"));
    QVERIFY(v2.action.size() >= 0); // no throw
}
```

- [ ] **Step 3: Add the .cpp to core** — in `CMakeLists.txt`, add `src/trading/ai_activity/AiActivityFormat.cpp` to the `openterminal_core` source list (near the other `src/trading/*.cpp` entries, ~line 1057).

- [ ] **Step 4: Build + run — expect FAIL** (`format_activity` undefined):
```
cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON >/dev/null
cmake --build /tmp/ot-build-test --target tst_ai_activity -j8
```

- [ ] **Step 5: Implement** `src/trading/ai_activity/AiActivityFormat.cpp`:
```cpp
#include "trading/ai_activity/AiActivityFormat.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace openmarketterminal::trading {

namespace {
const QSet<QString>& toast_decisions() {
    static const QSet<QString> s = {"filled","partially_filled","accepted","submitted","new",
                                    "open","cancelled","canceled","rejected","denied"};
    return s;
}
ActivityView::Severity severity_for(const QString& d) {
    using S = ActivityView::Severity;
    if (d=="filled"||d=="partially_filled"||d=="cancelled"||d=="canceled") return S::Success;
    if (d=="rejected"||d=="denied") return S::Error;
    return S::Info; // accepted/submitted/new/open + anything else
}
// Short human action from intent_json; falls back to "" if absent/garbled.
QString action_summary(const QString& intent_json) {
    const QJsonDocument doc = QJsonDocument::fromJson(intent_json.toUtf8());
    if (!doc.isObject()) return {};
    const QJsonObject o = doc.object();
    const QString side = o.value("side").toString().toUpper();
    const QString sym  = o.value("symbol").toString().toUpper();
    const double qty   = o.value("quantity").toVariant().toDouble();
    QStringList parts;
    if (!side.isEmpty()) parts << side;
    if (qty > 0) parts << QString::number(qty, 'g', 10);
    if (!sym.isEmpty()) parts << sym;
    return parts.join(' ');
}
} // namespace

ActivityView format_activity(const TradeAuditRow& row) {
    ActivityView v;
    const QString d = row.decision.trimmed().toLower();
    v.toast = (row.phase.compare("prepare", Qt::CaseInsensitive) != 0) && toast_decisions().contains(d);
    v.severity = severity_for(d);
    v.time = row.ts;
    v.tool = row.tool;
    v.account_mode = row.mode.isEmpty() ? row.account : (row.account + " · " + row.mode);
    v.action = action_summary(row.intent_json);
    v.decision = row.decision;
    v.reason = row.reason;
    const QString act = v.action.isEmpty() ? row.tool : (row.tool + " · " + v.action);
    v.message = QStringLiteral("AI · %1 → %2%3")
                    .arg(act, row.decision,
                         row.mode.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(row.mode));
    return v;
}

} // namespace openmarketterminal::trading
```

- [ ] **Step 6: Build + run — expect PASS** (all Task 2 slots + Task 1). Neuter-proof: change `!= 0` (prepare guard) to `== 0`; rebuild; confirm `formatNoToastForPrepareOrValidation` FAILS; restore. Then change the toast set to drop `"filled"`; confirm `formatToastsTerminalOutcomes` FAILS; restore.

- [ ] **Step 7: Commit**
```bash
git add src/trading/ai_activity/AiActivityFormat.h src/trading/ai_activity/AiActivityFormat.cpp tests/tst_ai_activity.cpp CMakeLists.txt
git commit -m "feat(activity): pure format_activity (toast filter + severity + message)"
```

---

## Task 3: `AiActivityNotifier` (main-thread bridge → toast + signal)

**Files:** Create `src/app/AiActivityNotifier.{h,cpp}`; Modify `CMakeLists.txt` (add to GUI sources — near other `src/app/*.cpp`). GUI wiring; build-verified (no unit test — the logic it relies on is `format_activity`, already tested).

- [ ] **Step 1: Create** `src/app/AiActivityNotifier.h`:
```cpp
#pragma once
#include "trading/ai_activity/AiActivityFormat.h"
#include <QObject>
#include <QVariantMap>

namespace openmarketterminal::app {

/// Lives on the main thread. Bridges the "trade.audit" EventBus event to the UI:
/// toasts terminal outcomes (any screen) and re-emits a typed view for the feed.
class AiActivityNotifier : public QObject {
    Q_OBJECT
  public:
    explicit AiActivityNotifier(QObject* parent = nullptr);
  signals:
    void activity(const openmarketterminal::trading::ActivityView& view);
  private:
    void on_event(const QString& event, const QVariantMap& data);
};

} // namespace openmarketterminal::app
```

- [ ] **Step 2: Create** `src/app/AiActivityNotifier.cpp`:
```cpp
#include "app/AiActivityNotifier.h"
#include "core/events/EventBus.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "ui/notifications/NotificationService.h"

namespace openmarketterminal::app {

using trading::ActivityView;

static ui::ToastService::Severity to_toast_sev(ActivityView::Severity s) {
    switch (s) {
        case ActivityView::Severity::Success: return ui::ToastService::Severity::Success;
        case ActivityView::Severity::Error:   return ui::ToastService::Severity::Error;
        case ActivityView::Severity::Warning: return ui::ToastService::Severity::Warning;
        case ActivityView::Severity::Info:    default: return ui::ToastService::Severity::Info;
    }
}

AiActivityNotifier::AiActivityNotifier(QObject* parent) : QObject(parent) {
    // Queued: append() fires on the bridge worker; this slot must run on the main thread.
    connect(&EventBus::instance(), &EventBus::eventPublished, this,
            &AiActivityNotifier::on_event, Qt::QueuedConnection);
}

void AiActivityNotifier::on_event(const QString& event, const QVariantMap& data) {
    if (event != QLatin1String("trade.audit")) return;
    const ActivityView v = trading::format_activity(audit_row_from_map(data));
    if (v.toast)
        ui::ToastService::instance().post(to_toast_sev(v.severity), v.message,
                                          QStringLiteral("ai-trading"));
    emit activity(v);
}

} // namespace openmarketterminal::app
```
(Verify the exact `ToastService` enum/post signature in `src/ui/notifications/NotificationService.h`; adjust the namespace/path if needed. Register `ActivityView` with `qRegisterMetaType<openmarketterminal::trading::ActivityView>("openmarketterminal::trading::ActivityView")` in the constructor so the queued signal can carry it — add `#include <QMetaType>`; this is REQUIRED for a queued signal with a custom type.)

- [ ] **Step 3: Add to CMake GUI sources** — add `src/app/AiActivityNotifier.cpp` to the GUI target source list in `CMakeLists.txt` (near other `src/app/*.cpp`). Confirm `ToastService`'s TU is in the same (GUI) target.

- [ ] **Step 4: Build the GUI — must link:**
```
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal -j8 2>&1 | tail -5
```
If `ToastService::post`'s signature differs, fix the call. If the queued signal complains about an unregistered metatype at runtime, ensure the `qRegisterMetaType` line is present.

- [ ] **Step 5: Commit**
```bash
git add src/app/AiActivityNotifier.h src/app/AiActivityNotifier.cpp CMakeLists.txt
git commit -m "feat(activity): AiActivityNotifier — trade.audit → toast + typed signal (main thread)"
```

---

## Task 4: `AiActivitySection` feed panel + Settings tab

**Files:** Create `src/screens/settings/AiActivitySection.{h,cpp}`; Modify `src/screens/settings/SettingsScreen.cpp`; Modify `CMakeLists.txt` (GUI sources). Build-verified.

- [ ] **Step 1: Study** `src/screens/polymarket/PolymarketActivityFeed.{h,cpp}` (QTableWidget setup, `append_trade` prepend+flash+cap, `set_trades`, retranslate) — mirror its structure.

- [ ] **Step 2: Create** `src/screens/settings/AiActivitySection.h`:
```cpp
#pragma once
#include "trading/ai_activity/AiActivityFormat.h"
#include <QWidget>
class QTableWidget;
namespace openmarketterminal::screens::settings {

/// Settings → "AI Activity" tab: scrollable log of every AI trading action.
/// Loads TradeAuditRepository::recent(200) on construction; prepends live rows
/// via add_activity() (connected to AiActivityNotifier::activity).
class AiActivitySection : public QWidget {
    Q_OBJECT
  public:
    explicit AiActivitySection(QWidget* parent = nullptr);
  public slots:
    void add_activity(const openmarketterminal::trading::ActivityView& view); // prepend + flash
  private:
    void load_recent();   // recent(200) → rows
    void insert_row(int at, const openmarketterminal::trading::ActivityView& view);
    QTableWidget* table_ = nullptr;
    static constexpr int kCap = 200;
};

} // namespace openmarketterminal::screens::settings
```

- [ ] **Step 3: Create** `src/screens/settings/AiActivitySection.cpp` — a `QTableWidget` with 6 columns (Time, Tool, Account/Mode, Action, Decision, Reason). `load_recent()` calls `TradeAuditRepository::instance().recent(kCap)`, maps each `TradeAuditRow` via `trading::format_activity`, and fills the table newest-first. `add_activity()` inserts at row 0, trims to `kCap`, and flashes the row background per `view.severity` (green/blue/red), mirroring `PolymarketActivityFeed::append_trade`. Color the Decision cell by severity. Follow the existing settings-section styling (look at `NotificationsSection.cpp` for the section frame/title pattern). Keep it focused — read-only table, no actions.

- [ ] **Step 4: Register the Settings tab** in `src/screens/settings/SettingsScreen.cpp`:
  - Add `#include "screens/settings/AiActivitySection.h"`.
  - Add a factory entry: `section_factories_[15] = [] { return new screens::settings::AiActivitySection; };` (use the next free index; the array currently goes to `[14]` GeneralSection — confirm the array size / nav-label list length and extend BOTH).
  - Add the nav label `"AI Activity"` to the section nav-label list (find the list that parallels `section_factories_`; add the matching entry at the same index). Grep for `"Security"` / `"Notifications"` in SettingsScreen.cpp to find the label list.

- [ ] **Step 5: Add to CMake GUI sources** — `src/screens/settings/AiActivitySection.cpp`.

- [ ] **Step 6: Build the GUI — must link + show the tab:**
```
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal -j8 2>&1 | tail -5
```

- [ ] **Step 7: Commit**
```bash
git add src/screens/settings/AiActivitySection.h src/screens/settings/AiActivitySection.cpp src/screens/settings/SettingsScreen.cpp CMakeLists.txt
git commit -m "feat(activity): Settings → AI Activity feed panel (recent + live prepend)"
```

---

## Task 5: Shell wiring + end-to-end manual verify

**Files:** Modify `src/app/WindowFrame*` (the shell). Build-verified + manual.

- [ ] **Step 1: Own the notifier at shell startup.** In the GUI shell bring-up (`WindowFrame_Setup.cpp` or wherever services are created on the main thread), instantiate `auto* ai_notifier_ = new app::AiActivityNotifier(this);` (parented to the WindowFrame so it lives on the main thread for the app's lifetime). Add the member to `WindowFrame.h` + the include.

- [ ] **Step 2: Connect the notifier to the live feed.** The `AiActivitySection` is created lazily by the SettingsScreen factory. Simplest robust wiring: have `AiActivitySection`'s constructor connect itself to the notifier's `activity` signal via a small accessor — i.e., expose the shell notifier through a singleton-style accessor OR have the section connect to a shared signal. RECOMMENDED: make `AiActivityNotifier` a process singleton (`AiActivityNotifier::instance()`), instantiated once at shell startup; `AiActivitySection` connects to `AiActivityNotifier::instance().activity` in its constructor (and the toast path works regardless of whether the panel is open). Adjust Task 3 to add `static AiActivityNotifier& instance();` if you take this route. Confirm the toast fires even when the panel was never opened (the notifier is alive from startup).

- [ ] **Step 3: Build + selftest:**
```
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli -j8 2>&1 | tail -5
```
Run any GUI `--selftest-*` that exercises startup to confirm no regression.

- [ ] **Step 4: Manual end-to-end (document for the user, execute together):** launch the fresh GUI; with the Alpaca paper account armed, run `openterminalcli mcp call fast_submit_order '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":310}'` → **expect a toast** ("AI · fast_submit_order · BUY 1 AAPL → accepted (live)") on whatever screen is showing, AND the row appears in **Settings → AI Activity**. Then `cancel_order` → another toast + row.

- [ ] **Step 5: Commit**
```bash
git add src/app/WindowFrame.h src/app/WindowFrame_Setup.cpp src/app/AiActivityNotifier.h src/app/AiActivityNotifier.cpp
git commit -m "feat(activity): wire AiActivityNotifier into the shell (toast on any screen)"
```

---

## Final review (after all tasks)
Dispatch a reviewer to confirm: (a) `format_activity` matches the locked toast rule exactly; (b) the notifier runs on the main thread (queued connection) — no GUI calls from the bridge worker; (c) the emit in `append()` is fire-and-forget (cannot break the audit write); (d) no regression in the trading/bridge suites (`ctest -R "tst_ai_activity|tst_fast_live|tst_order_flow|tst_settings_gate"`). Then `superpowers:finishing-a-development-branch`.

---

## Self-review (against the spec)
- **Spec coverage:** emit (T1), pure formatter + locked toast rule (T2), notifier/toast main-thread (T3), feed panel + Settings tab (T4), shell wiring + any-screen toast (T5), tests (T1/T2 pure + emit). ✓
- **Placeholder scan:** every code step has concrete code; the only "study/grep" steps are T4 (mirror PolymarketActivityFeed; find the nav-label list) — intentional pattern-matching, not a placeholder. ✓
- **Type consistency:** `ActivityView{toast,severity,message,time,tool,account_mode,action,decision,reason}`, `format_activity(TradeAuditRow)`, `audit_row_to_map`/`audit_row_from_map`, `AiActivityNotifier::activity(ActivityView)`, `AiActivitySection::add_activity(ActivityView)`, event name `"trade.audit"` — consistent across all tasks. ✓
- **Threading:** queued connection (T3) + qRegisterMetaType for the custom-type queued signal (T3) — called out. ✓
