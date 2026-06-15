// tst_trading_gate_keystone.cpp — the SECURITY KEYSTONE of the AI-trading
// substrate: a CLI/AI agent must NEVER be able to arm or enable its own
// trading, or raise its own risk caps, via the settings-WRITE tool path.
//
// Three layers, all against a real opened DB under a QTemporaryDir HOME
// (mirrors tst_settings_gate's HeadlessRuntime bring-up):
//
//   1. Reader helpers — cli_paper_trading_allowed() / cli_live_armed() default
//      false (missing key = false) and return true only after a literal "true".
//
//   2. Classification — is_gui_only_setting() matches every `cli.*` knob
//      (trading toggles AND cli.risk.* caps) and nothing outside the namespace.
//
//   3. KEYSTONE — even with cli.allow_settings_write=true (so the headless
//      settings-write GATE passes and the set_setting handler actually runs),
//      set_setting MUST refuse any `cli.*` key with a "GUI-only" failure AND
//      leave the DB unchanged. Settings-write ON is the whole point: it
//      discriminates the denylist refusal from a vacuous gate refusal. We assert
//      the refusal message ("GUI-only") AND a sentinel-default read (proving no
//      write happened, not merely that the value still equals a default).

#include <QtTest>
#include <QTemporaryDir>
#include <QJsonObject>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/SettingsRepository.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstTradingGateKeystone : public QObject {
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

    // ── Layer 1: reader helpers default false, true only after literal "true" ──
    void readers_default_false_then_true() {
        // Fresh DB: neither key set ⇒ both readers closed.
        QVERIFY2(!mcp::cli_paper_trading_allowed(), "cli_paper_trading_allowed must default false");
        QVERIFY2(!mcp::cli_live_armed(), "cli_live_armed must default false");

        set_key("cli.allow_paper_trading", "true");
        QVERIFY(mcp::cli_paper_trading_allowed());
        set_key("cli.live_trading_armed", "true");
        QVERIFY(mcp::cli_live_armed());

        // Non-"true" keeps them closed (default-deny on anything else).
        set_key("cli.allow_paper_trading", "false");
        set_key("cli.live_trading_armed", "no");
        QVERIFY2(!mcp::cli_paper_trading_allowed(), "\"false\" must keep paper-trading reader closed");
        QVERIFY2(!mcp::cli_live_armed(), "non-\"true\" must keep live-armed reader closed");
    }

    // ── Layer 2: is_gui_only_setting matches the whole cli.* namespace ──────────
    void gui_only_classification() {
        QVERIFY(mcp::is_gui_only_setting("cli.allow_trading"));
        QVERIFY(mcp::is_gui_only_setting("cli.live_trading_armed"));
        QVERIFY(mcp::is_gui_only_setting("cli.allow_paper_trading"));
        QVERIFY(mcp::is_gui_only_setting("cli.risk.max_order_value"));
        QVERIFY2(!mcp::is_gui_only_setting("general.theme"),
                 "non-cli key must NOT be GUI-only");
        // Case-variant evasion must NOT slip past the chokepoint. The match is
        // deliberately case-insensitive so it can't depend on downstream readers
        // normalising the key.
        QVERIFY2(mcp::is_gui_only_setting("CLI.allow_trading"),
                 "case-variant cli.* must still be GUI-only");
        QVERIFY2(mcp::is_gui_only_setting("Cli.Risk.Max_Order_Value"),
                 "mixed-case cli.* must still be GUI-only");
    }

    // ── Layer 3: THE KEYSTONE ──────────────────────────────────────────────────
    // settings-write permission ON, yet set_setting still refuses cli.* keys and
    // does not write them. A vacuous gate refusal is ruled out by requiring the
    // message to mention "GUI-only" (the denylist's words, not the gate's).
    void keystone_set_setting_refuses_cli_keys_even_with_write_permission() {
        // Permission FULLY granted — this is what an agent could turn on for
        // itself if the settings-write tool honoured cli.* writes.
        set_key("cli.allow_settings_write", "true");

        // Sanity: with the gate open, a NON-cli write succeeds (proves the gate
        // is genuinely open, so a later cli.* refusal is the denylist, not the
        // gate failing closed).
        {
            auto ok = rt_.call_tool(
                "set_setting",
                QJsonObject{{"key", "general.theme"}, {"value", "dark"}, {"category", "general"}});
            QVERIFY2(ok.success, qPrintable("non-cli write must succeed with gate open: " + ok.error));
        }

        // (a) cli.allow_trading — the arm-my-own-trading attack.
        {
            auto res = rt_.call_tool(
                "set_setting",
                QJsonObject{{"key", "cli.allow_trading"}, {"value", "true"}});
            QVERIFY2(!res.success, "set_setting must refuse cli.allow_trading even with write permission");
            QVERIFY2(res.error.contains("GUI-only"),
                     qPrintable("refusal must be the GUI-only denylist, not the gate: " + res.error));
            // Sentinel proves NO write happened — the row must never have been
            // created, so the default sentinel comes back verbatim.
            auto chk = SettingsRepository::instance().get("cli.allow_trading", "__unset__");
            QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                     qPrintable("cli.allow_trading must NOT have been written: "
                                + (chk.is_ok() ? chk.value() : QString("<err>"))));
        }

        // (b) cli.risk.max_order_value — the raise-my-own-risk-cap attack.
        {
            auto res = rt_.call_tool(
                "set_setting",
                QJsonObject{{"key", "cli.risk.max_order_value"}, {"value", "1000000"}});
            QVERIFY2(!res.success, "set_setting must refuse cli.risk.max_order_value even with write permission");
            QVERIFY2(res.error.contains("GUI-only"),
                     qPrintable("refusal must be the GUI-only denylist, not the gate: " + res.error));
            auto chk = SettingsRepository::instance().get("cli.risk.max_order_value", "__unset__");
            QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                     qPrintable("cli.risk.max_order_value must NOT have been written: "
                                + (chk.is_ok() ? chk.value() : QString("<err>"))));
        }

        // (c) Case-variant of a cli.* key — must ALSO be refused by the live
        // tool path (not just the classifier), and must not create a shadow row.
        {
            auto res = rt_.call_tool(
                "set_setting",
                QJsonObject{{"key", "CLI.allow_trading"}, {"value", "true"}});
            QVERIFY2(!res.success, "set_setting must refuse a case-variant cli.* key");
            QVERIFY2(res.error.contains("GUI-only"),
                     qPrintable("case-variant refusal must be the GUI-only denylist: " + res.error));
            auto chk = SettingsRepository::instance().get("CLI.allow_trading", "__unset__");
            QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                     qPrintable("case-variant cli.* must NOT have been written: "
                                + (chk.is_ok() ? chk.value() : QString("<err>"))));
        }

        set_key("cli.allow_settings_write", "false");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstTradingGateKeystone)
#include "tst_trading_gate_keystone.moc"
