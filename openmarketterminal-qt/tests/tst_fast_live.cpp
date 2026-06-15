// tst_fast_live.cpp — Fast Live Mode (Phase D), Task 1: the cli.fast_live_armed
// constitution flag + the is_fast_live_tool classifier + the fast-arm gate
// predicate. No live execution yet (the fast tools land in later tasks), so this
// pins down only the controls every later FAST task depends on:
//
//   1. Reader — cli_fast_live_armed() defaults false (missing key = false) and is
//      true only after a literal "true".
//
//   2. Classifier — is_fast_live_tool() matches EXACTLY the 7 fast-live names by
//      canonical name (true even though none are registered yet) and rejects the
//      submit_order / live_* / read paths.
//
//   3. KEYSTONE — even with cli.allow_settings_write=true, set_setting MUST refuse
//      cli.fast_live_armed with a "GUI-only" failure AND leave the DB unchanged
//      (sentinel-default read proves no write). The AI can never fast-arm itself.
//
//   4. GATING — the fast-arm gate predicate (cli_trading_allowed() &&
//      cli_live_armed() && cli_fast_live_armed()) is the IDENTICAL predicate the
//      three host auth-checkers apply. The fast tools don't exist yet, so we
//      assert the helpers + the predicate directly: base-live-armed but NOT
//      fast-armed → classifier true but predicate false; arm fast → predicate true.
//
// Bring-up mirrors tst_live_trading.cpp: a HeadlessRuntime init("default")
// registers migrations and opens the DB under a QTemporaryDir HOME before any
// repo/tool call.

#include <QtTest>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/SettingsRepository.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstFastLive : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;
    HeadlessRuntime rt_;

    static void set_key(const QString& key, const QString& val) {
        auto r = SettingsRepository::instance().set(key, val, "cli");
        QVERIFY2(r.is_ok(), "SettingsRepository::set failed — settings table absent?");
    }

    // The fast-arm gate predicate, duplicated verbatim from the three host
    // auth-checkers. Asserting it here pins the contract the hosts implement.
    static bool fast_gate_open() {
        return mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed();
    }

    void clear_keys() {
        for (const char* k : {"cli.allow_trading", "cli.live_trading_armed",
                              "cli.fast_live_armed", "cli.allow_settings_write"})
            SettingsRepository::instance().set(QString::fromLatin1(k), "false", "cli");
    }

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // ── KEYSTONE (run FIRST): set_setting refuses the GUI-only fast-arm key. ──
    // Ordered ahead of the reader/gating slots so its sentinel-default read sees a
    // key no earlier slot has written — the only way to prove the refused write
    // created no row at all (mirrors tst_live_trading's keystone slot).
    void keystone_set_setting_refuses_fast_live_armed() {
        set_key("cli.allow_settings_write", "true");
        auto res = rt_.call_tool(
            "set_setting", QJsonObject{{"key", "cli.fast_live_armed"}, {"value", "true"}});
        QVERIFY2(!res.success, "set_setting must refuse cli.fast_live_armed even with write permission");
        QVERIFY2(res.error.contains("GUI-only"),
                 qPrintable("refusal must be the GUI-only denylist, not the gate: " + res.error));
        auto chk = SettingsRepository::instance().get("cli.fast_live_armed", "__unset__");
        QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                 qPrintable("cli.fast_live_armed must NOT have been written: "
                            + (chk.is_ok() ? chk.value() : QString("<err>"))));
        set_key("cli.allow_settings_write", "false");
    }

    // ── Reader: defaults false, true only on a literal "true". ──
    void fast_live_armed_reader_default_false_then_true() {
        QVERIFY2(!mcp::cli_fast_live_armed(),
                 "cli_fast_live_armed must default false on a fresh key");
        set_key("cli.fast_live_armed", "true");
        QVERIFY(mcp::cli_fast_live_armed());
        set_key("cli.fast_live_armed", "false");
        QVERIFY2(!mcp::cli_fast_live_armed(), "\"false\" must keep fast-arm off");
    }

    // ── Classifier: matches EXACTLY the 7 fast-live names, nothing else. ──
    void is_fast_live_tool_matches_exactly_the_seven() {
        for (const char* name : {"fast_submit_order", "cancel_order", "replace_order",
                                 "exit_position", "get_positions", "get_open_orders",
                                 "get_fills"}) {
            QVERIFY2(mcp::is_fast_live_tool(QString::fromLatin1(name)),
                     qPrintable(QStringLiteral("is_fast_live_tool must match ") + name));
        }
        QVERIFY2(!mcp::is_fast_live_tool("submit_order"),
                 "submit_order is the slow gated path, not a fast-live tool");
        QVERIFY2(!mcp::is_fast_live_tool("live_place_order"),
                 "raw live_* tools must NOT be fast-live tools");
        QVERIFY2(!mcp::is_fast_live_tool("get_quote"), "read tools must NOT match");
    }

    // ── Gating: the fast-arm gate predicate the three hosts apply. ──
    void fast_gate_requires_all_three_arms() {
        // Base live armed but fast NOT armed: classifier matches yet the gate is
        // shut — a fast tool reaching any host checker would be DENIED.
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.fast_live_armed", "false");
        QVERIFY2(mcp::is_fast_live_tool("get_positions"), "classifier must still match");
        QVERIFY2(!fast_gate_open(),
                 "fast gate MUST be shut when base live is armed but fast is not");

        // Arm fast → the gate opens (all three predicates true).
        set_key("cli.fast_live_armed", "true");
        QVERIFY2(fast_gate_open(), "fast gate MUST open once fully fast-armed");

        // Revoke base live arm → gate shuts again even though fast stays armed.
        set_key("cli.live_trading_armed", "false");
        QVERIFY2(!fast_gate_open(), "fast gate MUST shut if base live arm is revoked");

        clear_keys();
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstFastLive)
#include "tst_fast_live.moc"
