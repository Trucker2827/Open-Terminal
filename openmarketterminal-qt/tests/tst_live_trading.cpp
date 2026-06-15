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
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "trading/BrokerRegistry.h"
#include "trading/TradingTypes.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

// ── FakeBroker — a minimal sandbox IBroker the live equity path can route to.
// place_order ALWAYS reports a successful fill with order_id "FAKE-1"; every
// other method is a harmless stub. Registered under a test broker id via
// BrokerRegistry::register_broker_for_test so an armed equity `submit_order
// live` against a "sandbox" account routes here instead of a real broker. NO
// real credentials are ever used.
class FakeBroker : public trading::IBroker {
  public:
    int place_order_calls = 0;

    trading::BrokerId id() const override { return trading::BrokerId::Alpaca; }
    const char* name() const override { return "FakeBroker"; }
    const char* base_url() const override { return "https://fake.invalid"; }
    trading::BrokerProfile profile() const override { return {}; }

    trading::TokenExchangeResponse exchange_token(const QString&, const QString&,
                                                  const QString&) override {
        return {true, "FAKE-TOKEN", "", "", "", ""};
    }

    trading::OrderPlaceResponse place_order(const trading::BrokerCredentials&,
                                            const trading::UnifiedOrder&) override {
        ++const_cast<FakeBroker*>(this)->place_order_calls;
        return {true, QStringLiteral("FAKE-1"), QString()};
    }
    trading::ApiResponse<QJsonObject> modify_order(const trading::BrokerCredentials&,
                                                   const QString&,
                                                   const QJsonObject&) override {
        return {};
    }
    trading::ApiResponse<QJsonObject> cancel_order(const trading::BrokerCredentials&,
                                                   const QString&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerOrderInfo>>
    get_orders(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QJsonObject>
    get_trade_book(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerPosition>>
    get_positions(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerHolding>>
    get_holdings(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<trading::BrokerFunds>
    get_funds(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerQuote>>
    get_quotes(const trading::BrokerCredentials&, const QVector<QString>&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerCandle>>
    get_history(const trading::BrokerCredentials&, const QString&, const QString&,
                const QString&, const QString&) override {
        return {};
    }

  protected:
    QMap<QString, QString> auth_headers(const trading::BrokerCredentials&) const override {
        return {};
    }
};

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

    // True iff a recent trade_audit row matches tool + mode + decision. Used to
    // prove the live fill was audited under mode "live".
    static bool audit_has_mode_decision(const QString& tool, const QString& mode,
                                        const QString& decision) {
        auto r = TradeAuditRepository::instance().recent(100);
        if (!r.is_ok())
            return false;
        for (const auto& row : r.value())
            if (row.tool == tool && row.mode == mode && row.decision == decision)
                return true;
        return false;
    }

    // Prepare a valid equity LIMIT draft via the prepare_order tool and return
    // its draft_id. A LIMIT order makes the resolved price deterministic (no
    // market-data dependency) and small qty*price stays under the risk floor.
    // `account` (when non-empty) is stamped into the intent for mismatch tests.
    QString prepare_equity_draft(const QString& account = QString()) {
        QJsonObject args{{"symbol", "AAPL"},     {"exchange", "NASDAQ"},
                         {"side", "buy"},        {"quantity", 10},
                         {"order_type", "limit"}, {"limit_price", 100.0}};
        if (!account.isEmpty())
            args["account"] = account;
        auto res = rt_.call_tool("prepare_order", args);
        if (!res.success)
            return QString();
        const QJsonObject d = res.data.toObject();
        if (d.value("status").toString() != QLatin1String("prepared"))
            return QString();
        return d.value("draft_id").toString();
    }

    // Submit a draft live and return the decision object.
    QJsonObject submit_live(const QString& draft_id) {
        auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        if (!res.success)
            return QJsonObject{{"status", "TOOL_FAIL"}, {"reason", res.error}};
        return res.data.toObject();
    }

    // Clear every cli.* knob the live slots toggle, restoring fresh defaults so
    // later reader/keystone slots still see a clean DB (QtTest runs in order).
    void clear_live_keys() {
        for (const char* k : {"cli.allow_trading", "cli.live_trading_armed",
                              "cli.allowed_account", "cli.kill_switch"})
            SettingsRepository::instance().set(QString::fromLatin1(k), "false", "cli");
        SettingsRepository::instance().set("cli.allowed_account", "", "cli");
    }

    // Register the FakeBroker + a sandbox account, returning the account id.
    QString setup_fake_account() {
        trading::BrokerRegistry::instance().register_broker_for_test(
            "fake", std::make_unique<FakeBroker>());
        auto acct = trading::AccountManager::instance().add_account("fake", "Test");
        trading::AccountManager::instance().set_trading_mode(acct.account_id, "sandbox");
        trading::BrokerCredentials creds;
        creds.broker_id = "fake";
        creds.access_token = "FAKE-TOKEN"; // non-empty so the live path reaches the broker
        trading::AccountManager::instance().save_credentials(acct.account_id, creds);
        return acct.account_id;
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

    // ── Phase C, Task 3: gated LIVE equity execution ────────────────────────
    //
    // Every slot below proves place_order is reachable ONLY inside the full gate
    // stack: a single failing gate yields a recorded rejection and NEVER touches
    // the broker. The happy slot proves the gated fill routes through the
    // FakeBroker, records a live position, and audits mode "live".

    // (a) Not armed → DENIED at the gate stack. The headless auth-checker
    //     (HeadlessRuntime.cpp) gates submit_order live on the IDENTICAL
    //     predicate cli_trading_allowed() && cli_live_armed(), so an un-armed
    //     live submit is refused at the OUTER layer (res.success=false) before
    //     the handler's own gate-1 (defense-in-depth) is even reached. The
    //     safety property is: an un-armed live submit NEVER executes.
    void live_not_armed_rejected() {
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        // allow_trading on but live NOT armed → the && fails at the auth-checker.
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "false");
        auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", id}, {"mode", "live"}});
        QVERIFY2(!res.success, "un-armed live submit must be denied at the gate stack");
        clear_live_keys();
    }

    // (b) Armed but no allowed account configured → rejected.
    void live_no_allowed_account_rejected() {
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", ""); // default-deny
        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(),
                 QStringLiteral("no allowed account configured for AI trading"));
        clear_live_keys();
    }

    // (c) Armed + a valid allowed account, but the draft NAMES a different
    //     account → rejected (the AI may trade only the human-named account).
    void live_account_mismatch_rejected() {
        const QString acct = setup_fake_account();
        const QString id = prepare_equity_draft("some-other-account");
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);
        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(),
                 QStringLiteral("account not allowed for AI trading"));
        clear_live_keys();
    }

    // (d) Daily-loss halt: a pre-seeded big realized loss trips the daily-loss
    //     gate BEFORE any broker call → rejected.
    void live_daily_loss_halt_rejected() {
        const QString acct = setup_fake_account();
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        // Seed a huge realized loss for today so today_loss + max_loss > cap.
        LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), -1000000.0);
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);
        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("daily loss limit reached"));
        QVERIFY2(audit_has("submit_order", "daily loss limit reached"),
                 "daily-loss refusal must be audited");
        clear_live_keys();
    }

    // (d2) FIX B regression: a SMALL realized loss must NOT reject every order.
    //     Seed -100 (today_loss 100, default cap 5000). The order's worst-case
    //     loss is its notional (10*100=1000); 100+1000 <= 5000 → filled. Before
    //     the fix the live equity path fed daily_loss_ok the daily CAP (5000):
    //     100+5000 > 5000 → wrongly rejected the instant ANY loss existed. The
    //     per-slot init() wipe of daily_pnl means no manual seed-undo. This slot
    //     is RED on the pre-fix risk_floor_check (max_loss=cap), GREEN on the fix
    //     (max_loss=order_value). ──
    void live_small_loss_still_fills_within_cap() {
        const QString acct = setup_fake_account();
        const QString id = prepare_equity_draft(); // order_value 10*100 = 1000
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), -100.0);
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);
        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        clear_live_keys();
    }

    // (e) Kill switch DOMINATES: even fully armed with a valid account, the
    //     panic button short-circuits at the handler top → "kill switch engaged".
    void live_kill_switch_dominates() {
        const QString acct = setup_fake_account();
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);
        set_key("cli.kill_switch", "true");
        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        clear_live_keys();
    }

    // (f) HAPPY PATH: armed + valid sandbox account + FakeBroker → fill. The
    //     order routes through the fake (order_id "FAKE-1"), a live position is
    //     recorded at the resolved price, and the fill is audited mode "live".
    void live_happy_path_fills_via_fake_broker() {
        const QString acct = setup_fake_account();
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);

        const QJsonObject d = submit_live(id);
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(d.value("order_id").toString(), QStringLiteral("FAKE-1"));
        QCOMPARE(d.value("account").toString(), acct);
        QCOMPARE(d.value("mode").toString(), QStringLiteral("live"));

        // A live position row recorded at the resolved limit price (100) × qty (10).
        auto pos = LivePnlRepository::instance().get_open(acct, "equity", "AAPL");
        QVERIFY2(pos.is_ok() && pos.value().has_value(),
                 "the fill must record an OPEN live_positions row");
        QCOMPARE(pos.value()->qty, 10.0);
        QVERIFY2(qFuzzyCompare(pos.value()->avg_cost, 100.0), "recorded at resolved price 100");

        QVERIFY2(audit_has_mode_decision("submit_order", "live", "filled"),
                 "the live fill must be audited under mode 'live'");
        clear_live_keys();
    }

    // (h) The DIRECT live_* broker tools must NEVER be an ungated path to real
    //     money. Even fully armed (allow_trading + live_trading_armed + an allowed
    //     account), live_place_order over the headless host is DENIED by the
    //     auth-checker — the AI's ONLY live path is the gated submit_order. (Before
    //     the headless checker was reconciled with the daemon, this fired a real
    //     order gated by cli.allow_trading alone, bypassing the whole constitution.)
    void live_direct_tool_denied_even_when_armed() {
        const QString acct = setup_fake_account();
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);

        auto res = rt_.call_tool("live_place_order",
                                 QJsonObject{{"account_id", acct}, {"symbol", "AAPL"},
                                             {"side", "buy"}, {"quantity", 1}});
        QVERIFY2(!res.success,
                 "direct live_place_order MUST be denied even when fully armed — it "
                 "bypasses the kill-switch/armed/allowed-account/daily-loss stack");
        // And it left no live position (no broker call happened).
        auto pos = LivePnlRepository::instance().get_open(acct, "equity", "AAPL");
        QVERIFY2(pos.is_ok() && !pos.value().has_value(),
                 "denied direct live tool must not have recorded a fill");
        clear_live_keys();
    }

    // (g) Revocable: a draft prepared while armed must STILL be denied — with NO
    //     execution — if the human un-arms before submit. Every gate is re-read
    //     live, so revoking arming after prepare halts the order. As with (a),
    //     the auth-checker fires first (res.success=false); we additionally prove
    //     NO broker call / NO fill happened (no live position, draft untouched).
    void live_revocable_unarm_mid_flight_rejected() {
        const QString acct = setup_fake_account();
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.allowed_account", acct);
        const QString id = prepare_equity_draft();
        QVERIFY2(!id.isEmpty(), "prepare_equity_draft must yield a draft");
        // Human un-arms AFTER prepare, BEFORE submit.
        set_key("cli.live_trading_armed", "false");
        auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", id}, {"mode", "live"}});
        QVERIFY2(!res.success, "un-arming before submit must deny the live order");
        // No execution: no live position was opened, so nothing reached the broker.
        auto pos = LivePnlRepository::instance().get_open(acct, "equity", "AAPL");
        QVERIFY2(pos.is_ok() && !pos.value().has_value(),
                 "a revoked live submit must NOT record a fill");
        clear_live_keys();
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstLiveTrading)
#include "tst_live_trading.moc"
