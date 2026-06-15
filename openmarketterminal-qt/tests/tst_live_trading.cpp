// tst_live_trading.cpp — Phase C, Task 1: the kill switch + allowed-account
// constitution. Paper-safe (no live execution yet), but the controls these
// tests pin down are what every later LIVE task depends on:
//
//   1. Reader helpers — cli_kill_switch_engaged() defaults false (missing key =
//      false) and is true only after a literal "true"; cli_allowed_account()
//      defaults "" and round-trips a human-named account id (trimmed).
//
//   2. Enforcement — with cli.kill_switch=true, BOTH order-flow handlers
//      (prepare_order AND submit_order) short-circuit to status "rejected" with
//      reason "kill switch engaged", halting ALL AI trading.
//
//   3. KEYSTONE — even with cli.allow_settings_write=true, set_setting MUST
//      refuse cli.kill_switch and cli.allowed_account with a "GUI-only" failure
//      AND leave the DB unchanged (sentinel-default read proves no write).
//
// Bring-up mirrors tst_trading_gate_keystone.cpp / tst_pm_paper.cpp: a
// HeadlessRuntime init("default") registers migrations and opens the DB under a
// QTemporaryDir HOME before any repo/tool call.

#include <QtTest>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/SettingsRepository.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstLiveTrading : public QObject {
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

    // ── Layer 3 (run FIRST): KEYSTONE — set_setting refuses these GUI-only keys.
    // Ordered ahead of the reader/enforcement slots so its sentinel-default reads
    // see keys that no earlier slot has written — the only way to prove the
    // refused write created no row at all (mirrors tst_trading_gate_keystone). ──
    void keystone_set_setting_refuses_kill_switch_and_allowed_account() {
        set_key("cli.allow_settings_write", "true");

        // cli.kill_switch — an agent must never be able to DISENGAGE (or engage)
        // the panic button for itself.
        {
            auto res = rt_.call_tool(
                "set_setting", QJsonObject{{"key", "cli.kill_switch"}, {"value", "false"}});
            QVERIFY2(!res.success, "set_setting must refuse cli.kill_switch even with write permission");
            QVERIFY2(res.error.contains("GUI-only"),
                     qPrintable("refusal must be the GUI-only denylist, not the gate: " + res.error));
            auto chk = SettingsRepository::instance().get("cli.kill_switch", "__unset__");
            QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                     qPrintable("cli.kill_switch must NOT have been written: "
                                + (chk.is_ok() ? chk.value() : QString("<err>"))));
        }

        // cli.allowed_account — an agent must never be able to name its own
        // live-trade account.
        {
            auto res = rt_.call_tool(
                "set_setting", QJsonObject{{"key", "cli.allowed_account"}, {"value", "acct-evil"}});
            QVERIFY2(!res.success, "set_setting must refuse cli.allowed_account even with write permission");
            QVERIFY2(res.error.contains("GUI-only"),
                     qPrintable("refusal must be the GUI-only denylist, not the gate: " + res.error));
            auto chk = SettingsRepository::instance().get("cli.allowed_account", "__unset__");
            QVERIFY2(chk.is_ok() && chk.value() == "__unset__",
                     qPrintable("cli.allowed_account must NOT have been written: "
                                + (chk.is_ok() ? chk.value() : QString("<err>"))));
        }

        set_key("cli.allow_settings_write", "false");
    }

    // ── Layer 1: reader helpers ─────────────────────────────────────────────
    void kill_switch_reader_default_false_then_true() {
        QVERIFY2(!mcp::cli_kill_switch_engaged(),
                 "cli_kill_switch_engaged must default false on a fresh DB");
        set_key("cli.kill_switch", "true");
        QVERIFY(mcp::cli_kill_switch_engaged());
        // Anything other than a literal "true" keeps it disengaged.
        set_key("cli.kill_switch", "false");
        QVERIFY2(!mcp::cli_kill_switch_engaged(), "\"false\" must keep the kill switch off");
    }

    void allowed_account_reader_default_empty_then_set() {
        QVERIFY2(mcp::cli_allowed_account().isEmpty(),
                 "cli_allowed_account must default empty (default-deny)");
        set_key("cli.allowed_account", "  acct-1  ");
        QCOMPARE(mcp::cli_allowed_account(), QStringLiteral("acct-1")); // trimmed
    }

    // ── Layer 2: enforcement — kill switch halts BOTH handlers ───────────────
    void kill_switch_rejects_prepare_order() {
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"symbol", "AAPL"}, {"side", "buy"}, {"quantity", 1},
                        {"order_type", "market"}});
        QVERIFY2(res.success, qPrintable("kill-switch refusal is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(data.value("reason").toString(), QStringLiteral("kill switch engaged"));
        set_key("cli.kill_switch", "false");
    }

    void kill_switch_rejects_submit_order() {
        set_key("cli.kill_switch", "true");
        // Any args: the kill switch must short-circuit BEFORE the draft load, so
        // a non-existent draft_id still yields the kill-switch rejection.
        auto res = rt_.call_tool(
            "submit_order",
            QJsonObject{{"draft_id", "no-such-draft"}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("kill-switch refusal is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(data.value("reason").toString(), QStringLiteral("kill switch engaged"));
        set_key("cli.kill_switch", "false");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstLiveTrading)
#include "tst_live_trading.moc"
