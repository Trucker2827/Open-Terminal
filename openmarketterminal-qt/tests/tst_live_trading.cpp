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
#include "mcp/tools/LivePnl.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "storage/sqlite/Database.h"

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

    // True iff a recent trade_audit row matches the given tool + reason. Used to
    // prove a refusal was actually recorded (a NULL account would drop the row).
    static bool audit_has(const QString& tool, const QString& reason) {
        auto r = TradeAuditRepository::instance().recent(100);
        if (!r.is_ok())
            return false;
        for (const auto& row : r.value())
            if (row.tool == tool && row.reason == reason)
                return true;
        return false;
    }

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // Wipe the live-P&L tables before EACH slot so every P&L assertion can use
    // absolute spec values: daily_pnl is ONE cumulative row per UTC day shared by
    // the whole run, and live_positions rows persist. The kill-switch / keystone
    // slots never touch these tables, so the wipe is harmless for them. Runs after
    // initTestCase, so the v051 tables already exist.
    void init() {
        Database::instance().execute("DELETE FROM daily_pnl", {});
        Database::instance().execute("DELETE FROM live_positions", {});
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
        // The refusal MUST be audited (account is "" not NULL → the NOT NULL bind
        // succeeds; a dropped row would mean the halt left no trail).
        QVERIFY2(audit_has("prepare_order", "kill switch engaged"),
                 "kill-switch prepare refusal must write a trade_audit row");
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
        QVERIFY2(audit_has("submit_order", "kill switch engaged"),
                 "kill-switch submit refusal must write a trade_audit row");
        set_key("cli.kill_switch", "false");
    }

    // ── Phase C, Task 2: live realized-P&L ledger + daily-loss gate ──────────
    void realized_pnl_round_trip_records_loss() {
        mcp::tools::record_open("acct", "equity", "AAPL", 100, 150.0);
        const double realized = mcp::tools::record_close("acct", "equity", "AAPL", 100, 140.0);
        QVERIFY2(qFuzzyCompare(realized, -1000.0), "(140-150)*100 = -1000");
        auto t = LivePnlRepository::instance().realized_today(mcp::tools::today_utc());
        QVERIFY(t.is_ok());
        QVERIFY2(qFuzzyCompare(t.value(), -1000.0), "daily_pnl tally must hold -1000");
    }

    // record_open into an EXISTING open position must re-blend the average (the
    // path T3/T4 hit when the AI adds to a held live position) — a wrong avg here
    // would mis-state realized P&L and thus the daily-loss gate.
    void record_open_weighted_average_into_existing_position() {
        mcp::tools::record_open("acct", "equity", "MSFT", 100, 150.0);
        mcp::tools::record_open("acct", "equity", "MSFT", 50, 180.0);
        auto p = LivePnlRepository::instance().get_open("acct", "equity", "MSFT");
        QVERIFY(p.is_ok() && p.value().has_value());
        QCOMPARE(p.value()->qty, 150.0);
        QVERIFY2(qFuzzyCompare(p.value()->cost_basis, 24000.0), "100*150 + 50*180 = 24000");
        QVERIFY2(qFuzzyCompare(p.value()->avg_cost, 160.0), "24000/150 = 160 (re-blended, not 150)");
    }

    void daily_loss_gate_blocks_at_cap_after_loss() {
        mcp::tools::record_open("acct", "equity", "AAPL", 100, 150.0);
        mcp::tools::record_close("acct", "equity", "AAPL", 100, 140.0);  // realized -1000
        // today_loss = 1000, default cap = 5000.
        QVERIFY2(mcp::tools::daily_loss_ok(3000.0), "1000+3000 <= 5000 must pass");
        QVERIFY2(!mcp::tools::daily_loss_ok(4001.0), "1000+4001 > 5000 must block");
    }

    void daily_loss_gate_profit_floors_today_loss_at_zero() {
        mcp::tools::record_open("acct", "equity", "AAPL", 100, 140.0);
        const double realized = mcp::tools::record_close("acct", "equity", "AAPL", 100, 150.0);
        QVERIFY2(qFuzzyCompare(realized, 1000.0), "(150-140)*100 = +1000");
        // Profit must NOT expand headroom into negative — today_loss floors at 0.
        QVERIFY2(mcp::tools::daily_loss_ok(5000.0), "0+5000 <= 5000 must pass");
        QVERIFY2(!mcp::tools::daily_loss_ok(5001.0), "0+5001 > 5000 must block");
    }

    void partial_close_reduces_qty_and_cost_basis_prorata() {
        mcp::tools::record_open("acct", "equity", "AAPL", 100, 150.0);
        const double realized = mcp::tools::record_close("acct", "equity", "AAPL", 40, 150.0);
        QVERIFY2(qFuzzyIsNull(realized), "close at avg_cost yields 0 realized");
        auto pos = LivePnlRepository::instance().get_open("acct", "equity", "AAPL");
        QVERIFY(pos.is_ok() && pos.value().has_value());
        QVERIFY2(qFuzzyCompare(pos.value().value().qty, 60.0), "100-40 = 60 left");
        QVERIFY2(qFuzzyCompare(pos.value().value().cost_basis, 60.0 * 150.0),
                 "cost_basis pro-rata = 60*150 = 9000");
    }

    void close_then_reopen_same_instrument_no_unique_conflict() {
        mcp::tools::record_open("acct", "equity", "AAPL", 100, 150.0);
        mcp::tools::record_close("acct", "equity", "AAPL", 100, 150.0);  // fully closed
        auto closed = LivePnlRepository::instance().get_open("acct", "equity", "AAPL");
        QVERIFY(closed.is_ok());
        QVERIFY2(!closed.value().has_value(), "fully-closed position leaves no OPEN row");
        // Re-open the same instrument: must create a fresh OPEN row (no UNIQUE clash).
        mcp::tools::record_open("acct", "equity", "AAPL", 50, 200.0);
        auto reopened = LivePnlRepository::instance().get_open("acct", "equity", "AAPL");
        QVERIFY(reopened.is_ok() && reopened.value().has_value());
        QVERIFY2(qFuzzyCompare(reopened.value().value().qty, 50.0), "re-opened qty = 50");
        QVERIFY2(qFuzzyCompare(reopened.value().value().avg_cost, 200.0), "re-opened avg = 200");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstLiveTrading)
#include "tst_live_trading.moc"
