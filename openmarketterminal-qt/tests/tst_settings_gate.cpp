// tst_settings_gate.cpp — the two CLI capability gates (Task 6).
//
// Two layers, both against a real opened DB under a QTemporaryDir HOME:
//
//   1. Helper unit tests — cli_trading_allowed() / cli_settings_write_allowed()
//      default false (missing/unset key = false), and return true only after the
//      key is written literally "true".
//
//   2. HeadlessRuntime end-to-end (the LIVE enforcement path) — call_tool()
//      dispatches the handler + auth-checker on a worker thread, exactly as the
//      real `--headless mcp call` path does. We assert the gate DECISION by its
//      observable effect:
//        • settings-write (set_setting): denied + NOT written when the gate is
//          off; succeeds + persisted when on. (Asserting persistence, not just
//          res.success, proves the worker-thread DB read saw the flipped flag —
//          the cross-thread read could otherwise silently return the default.)
//        • trading/destructive (a probe tool, category != "settings"): denied
//          when cli.allow_trading off; runs when on.
//
// Singleton lifetime mirrors tst_headless_runtime: init ONCE in initTestCase().

#include <QtTest>
#include <QTemporaryDir>
#include <QJsonObject>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/SettingsRepository.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstSettingsGate : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;
    HeadlessRuntime rt_;

    static void set_key(const QString& key, const QString& val) {
        auto r = SettingsRepository::instance().set(key, val, "cli");
        QVERIFY2(r.is_ok(), "SettingsRepository::set failed — settings table absent?");
    }

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // ── Layer 1: helpers ────────────────────────────────────────────────────
    void helpers_default_false() {
        // Fresh DB: neither key set ⇒ both gates closed.
        QVERIFY2(!mcp::cli_trading_allowed(), "cli_trading_allowed must default false");
        QVERIFY2(!mcp::cli_settings_write_allowed(),
                 "cli_settings_write_allowed must default false");
    }

    void helpers_true_after_write() {
        set_key("cli.allow_trading", "true");
        set_key("cli.allow_settings_write", "true");
        QVERIFY(mcp::cli_trading_allowed());
        QVERIFY(mcp::cli_settings_write_allowed());

        // Non-"true" values do NOT open the gate (default-deny on anything else).
        set_key("cli.allow_trading", "false");
        set_key("cli.allow_settings_write", "yes");
        QVERIFY2(!mcp::cli_trading_allowed(), "\"false\" must keep gate closed");
        QVERIFY2(!mcp::cli_settings_write_allowed(), "non-\"true\" must keep gate closed");

        // Reset to closed for the e2e cases below.
        set_key("cli.allow_trading", "false");
        set_key("cli.allow_settings_write", "false");
    }

    // set_setting must be classified as a settings-WRITE tool; get_setting must not.
    void classification_rule() {
        QVERIFY2(mcp::is_settings_write_tool("set_setting"),
                 "set_setting must be a settings-write tool");
        QVERIFY2(!mcp::is_settings_write_tool("get_setting"),
                 "get_setting (read) must NOT be a settings-write tool");
    }

    // ── Layer 2: headless enforcement (live worker-thread path) ──────────────
    void settings_write_gate_denies_when_off() {
        set_key("cli.allow_settings_write", "false");
        auto res = rt_.call_tool(
            "set_setting",
            QJsonObject{{"key", "gate.probe_off"}, {"value", "v1"}, {"category", "general"}});
        QVERIFY2(!res.success, "set_setting must be denied while cli.allow_settings_write=false");

        // And it must NOT have written (sentinel default reveals a miss).
        auto chk = SettingsRepository::instance().get("gate.probe_off", "__missing__");
        QVERIFY2(chk.is_ok() && chk.value() == "__missing__",
                 qPrintable("denied write still persisted: "
                            + (chk.is_ok() ? chk.value() : QString("<err>"))));
    }

    void settings_write_gate_allows_when_on() {
        set_key("cli.allow_settings_write", "true");
        auto res = rt_.call_tool(
            "set_setting",
            QJsonObject{{"key", "gate.probe_on"}, {"value", "v2"}, {"category", "general"}});
        QVERIFY2(res.success, qPrintable("set_setting must succeed when gate on: " + res.error));

        // Crucial: the value persisted — proves the worker-thread checker read the
        // flipped flag (not a stale/default cross-thread read) AND the tool ran.
        auto chk = SettingsRepository::instance().get("gate.probe_on", "__missing__");
        QVERIFY2(chk.is_ok() && chk.value() == "v2",
                 qPrintable("gate on but value not persisted: "
                            + (chk.is_ok() ? chk.value() : QString("<err>"))));

        set_key("cli.allow_settings_write", "false");
    }

    void trading_gate_denies_then_allows() {
        // A destructive probe tool NOT in the settings category — routed to the
        // cli.allow_trading gate. Side-effect-free (records that it ran).
        static bool ran = false;
        mcp::ToolDef probe;
        probe.name = "tst_destructive_probe";
        probe.description = "test-only destructive tool";
        probe.category = "trading";
        probe.is_destructive = true;
        probe.handler = [](const QJsonObject&) -> mcp::ToolResult {
            ran = true;
            return mcp::ToolResult::ok("ran");
        };
        mcp::McpProvider::instance().register_tool(std::move(probe));

        // OFF → denied, handler never runs.
        set_key("cli.allow_trading", "false");
        ran = false;
        auto denied = rt_.call_tool("tst_destructive_probe", {});
        QVERIFY2(!denied.success, "destructive tool must be denied while cli.allow_trading=false");
        QVERIFY2(!ran, "denied destructive handler must not have run");

        // ON → permitted, handler runs.
        set_key("cli.allow_trading", "true");
        ran = false;
        auto allowed = rt_.call_tool("tst_destructive_probe", {});
        QVERIFY2(allowed.success, qPrintable("destructive tool must run when gate on: " + allowed.error));
        QVERIFY2(ran, "permitted destructive handler must have run");

        set_key("cli.allow_trading", "false");
        mcp::McpProvider::instance().unregister_tool("tst_destructive_probe");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstSettingsGate)
#include "tst_settings_gate.moc"
