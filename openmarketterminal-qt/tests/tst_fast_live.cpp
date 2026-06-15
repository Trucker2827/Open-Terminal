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
#include "mcp/McpProvider.h"
#include "mcp/tools/LivePnl.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "trading/BrokerRegistry.h"
#include "trading/TradingTypes.h"

#include <QJsonArray>

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

// ── FakeBroker — a minimal sandbox IBroker the fast-live READ tools route to.
// get_positions emits one fixture position; get_orders emits two fixture orders
// (one "filled", one "open") so get_fills / get_open_orders prove their status
// partition. Every other method is a harmless stub. NO real credentials.
class FakeBroker : public trading::IBroker {
  public:
    // De-risking call recorders (Task 3). Static so the test can read them after
    // the broker instance has been moved into the registry; reset in-slot.
    static inline int cancel_calls = 0;
    static inline QString last_cancel_id;
    static inline int close_calls = 0;
    static inline QString last_close_symbol;
    // One-shot place_order recorder (Task 4).
    static inline int place_calls = 0;
    static inline trading::UnifiedOrder last_order;
    // modify_order recorder (Task 5).
    static inline int modify_calls = 0;
    static inline QString last_modify_id;
    static inline QJsonObject last_mods;
    static void reset_derisk_counters() {
        cancel_calls = 0;
        last_cancel_id.clear();
        close_calls = 0;
        last_close_symbol.clear();
        place_calls = 0;
        last_order = trading::UnifiedOrder{};
        modify_calls = 0;
        last_modify_id.clear();
        last_mods = QJsonObject{};
    }

    trading::BrokerId id() const override { return trading::BrokerId::Alpaca; }
    const char* name() const override { return "FakeBroker"; }
    const char* base_url() const override { return "https://fake.invalid"; }
    trading::BrokerProfile profile() const override { return {}; }

    trading::TokenExchangeResponse exchange_token(const QString&, const QString&,
                                                  const QString&) override {
        return {true, "FAKE-TOKEN", "", "", "", ""};
    }
    trading::OrderPlaceResponse place_order(const trading::BrokerCredentials&,
                                            const trading::UnifiedOrder& order) override {
        ++place_calls;
        last_order = order;
        return {true, QStringLiteral("FAKE-1"), QString()};
    }
    // Modify — record + succeed (Task 5). Mirrors the cancel_order recorder shape;
    // the default {} returns success=false, which would mask a real "replaced".
    trading::ApiResponse<QJsonObject> modify_order(const trading::BrokerCredentials&,
                                                   const QString& order_id,
                                                   const QJsonObject& mods) override {
        ++modify_calls;
        last_modify_id = order_id;
        last_mods = mods;
        trading::ApiResponse<QJsonObject> resp;
        resp.success = true;
        return resp;
    }
    trading::ApiResponse<QJsonObject> cancel_order(const trading::BrokerCredentials&,
                                                   const QString& order_id) override {
        ++cancel_calls;
        last_cancel_id = order_id;
        trading::ApiResponse<QJsonObject> resp;
        resp.success = true;
        return resp;
    }
    // De-risking close — record + succeed. We override (rather than lean on the
    // IBroker default, which requires p.exchange == exchange and would return
    // "Position not found" for exit_position{symbol:"AAPL"} with no exchange).
    trading::ApiResponse<trading::OrderPlaceResponse>
    close_position(const trading::BrokerCredentials&, const QString& symbol, const QString&,
                   const QString&) override {
        ++close_calls;
        last_close_symbol = symbol;
        return {true, trading::OrderPlaceResponse{true, QStringLiteral("CLOSE-1"), {}}, {}};
    }
    trading::ApiResponse<QVector<trading::BrokerOrderInfo>>
    get_orders(const trading::BrokerCredentials&) override {
        trading::BrokerOrderInfo filled;
        filled.order_id = "F1";
        filled.symbol = "AAPL";
        filled.side = "BUY";
        filled.quantity = 10;
        filled.filled_qty = 10;
        filled.avg_price = 100.5;
        filled.status = "filled";
        trading::BrokerOrderInfo open;
        open.order_id = "O1";
        open.symbol = "MSFT";
        open.side = "BUY";
        open.quantity = 5;
        open.filled_qty = 0;
        open.price = 200.0;
        open.status = "open";
        trading::ApiResponse<QVector<trading::BrokerOrderInfo>> resp;
        resp.success = true;
        resp.data = QVector<trading::BrokerOrderInfo>{filled, open};
        return resp;
    }
    trading::ApiResponse<QJsonObject>
    get_trade_book(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerPosition>>
    get_positions(const trading::BrokerCredentials&) override {
        trading::BrokerPosition p;
        p.symbol = "AAPL";
        p.exchange = "NASDAQ";
        p.quantity = 10;
        p.avg_price = 100.5;
        p.ltp = 105.0;
        p.pnl = 45.0;
        p.side = "BUY";
        trading::ApiResponse<QVector<trading::BrokerPosition>> resp;
        resp.success = true;
        resp.data = QVector<trading::BrokerPosition>{p};
        return resp;
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
                              "cli.fast_live_armed", "cli.allow_settings_write",
                              "cli.kill_switch"})
            SettingsRepository::instance().set(QString::fromLatin1(k), "false", "cli");
        SettingsRepository::instance().set("cli.allowed_account", "", "cli");
    }

    // Register the FakeBroker + a sandbox account, returning the account id.
    // Safe to call repeatedly (mirrors tst_live_trading::setup_fake_account).
    QString setup_fake_account() {
        trading::BrokerRegistry::instance().register_broker_for_test(
            "fake", std::make_unique<FakeBroker>());
        auto acct = trading::AccountManager::instance().add_account("fake", "Test");
        trading::AccountManager::instance().set_trading_mode(acct.account_id, "sandbox");
        trading::BrokerCredentials creds;
        creds.broker_id = "fake";
        creds.access_token = "FAKE-TOKEN"; // non-empty so the read path reaches the broker
        trading::AccountManager::instance().save_credentials(acct.account_id, creds);
        return acct.account_id;
    }

    // Fully fast-arm the constitution against the FakeBroker account, returning
    // that account id. (base trading + base live arm + the SECOND fast arm.)
    QString arm_fast_live() {
        const QString acct = setup_fake_account();
        set_key("cli.allow_trading", "true");
        set_key("cli.live_trading_armed", "true");
        set_key("cli.fast_live_armed", "true");
        set_key("cli.kill_switch", "false");
        set_key("cli.allowed_account", acct);
        return acct;
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

    // ════════════════════════════════════════════════════════════════════
    // Task 2: the fast-live READ tools (get_positions/get_open_orders/get_fills)
    // ════════════════════════════════════════════════════════════════════

    // ── CONTRACT: the 3 reads register with category "fast-live" (NOT
    // "live-trading") and is_destructive=false. This pairing is load-bearing:
    // category "live-trading" + destructive would make is_live_execution_tool
    // DENY them at the host before the fast-arm gate fires; and a non-None /
    // destructive read would trip the Phase 6.12 confirmation modal. ──
    void fast_live_reads_register_with_fast_live_category() {
        const auto tools = mcp::McpProvider::instance().audit_all_tools();
        int found = 0;
        for (const char* nm : {"get_positions", "get_open_orders", "get_fills"}) {
            bool seen = false;
            for (const auto& t : tools) {
                if (t.name != QLatin1String(nm))
                    continue;
                seen = true;
                ++found;
                QCOMPARE(t.category, QStringLiteral("fast-live"));
                QVERIFY2(!t.is_destructive, qPrintable(QString(nm) + " read must be non-destructive"));
                QVERIFY2(t.has_handler, qPrintable(QString(nm) + " must have a handler"));
            }
            QVERIFY2(seen, qPrintable(QString("tool not registered: ") + nm));
        }
        QCOMPARE(found, 3);
    }

    // ── HAPPY: fully fast-armed → get_positions returns the fixture position. ──
    void get_positions_armed_returns_fixture() {
        arm_fast_live();
        auto res = rt_.call_tool("get_positions", QJsonObject{});
        QVERIFY2(res.success, qPrintable("armed get_positions must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QVERIFY2(!d.contains("status"), "armed read must not be a structured rejection");
        const QJsonArray positions = d.value("positions").toArray();
        QCOMPARE(positions.size(), 1);
        QCOMPARE(positions.at(0).toObject().value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(positions.at(0).toObject().value("quantity").toDouble(), 10.0);
        clear_keys();
    }

    // ── HAPPY: get_open_orders → only the non-terminal ("open") order. ──
    void get_open_orders_armed_filters_to_working() {
        arm_fast_live();
        auto res = rt_.call_tool("get_open_orders", QJsonObject{});
        QVERIFY2(res.success, qPrintable("armed get_open_orders must succeed: " + res.error));
        const QJsonArray orders = res.data.toObject().value("orders").toArray();
        QCOMPARE(orders.size(), 1);
        QCOMPARE(orders.at(0).toObject().value("order_id").toString(), QStringLiteral("O1"));
        QCOMPARE(orders.at(0).toObject().value("status").toString(), QStringLiteral("open"));
        clear_keys();
    }

    // ── HAPPY: get_fills → only the filled order (complement of open_orders). ──
    void get_fills_armed_filters_to_filled() {
        arm_fast_live();
        auto res = rt_.call_tool("get_fills", QJsonObject{});
        QVERIFY2(res.success, qPrintable("armed get_fills must succeed: " + res.error));
        const QJsonArray fills = res.data.toObject().value("fills").toArray();
        QCOMPARE(fills.size(), 1);
        QCOMPARE(fills.at(0).toObject().value("order_id").toString(), QStringLiteral("F1"));
        QCOMPARE(fills.at(0).toObject().value("status").toString(), QStringLiteral("filled"));
        clear_keys();
    }

    // ── GATE: un-arm the SECOND fast arm → handler-gate rejection. The host
    // auth-checker is BYPASSED for these reads (McpProvider only consults it when
    // auth_required != None || is_destructive, and the reads are None +
    // non-destructive), so the authoritative gate is the handler's
    // fast_live_gate(): all three arms must hold. Account stays set + kill switch
    // off, so gate check #2 (the arms) is the one that fires — asserting the
    // specific reason proves it's the ARMS check, not the account/kill checks. ──
    void get_positions_not_fast_armed_rejected_in_handler() {
        arm_fast_live();
        set_key("cli.fast_live_armed", "false"); // base live still armed
        auto res = rt_.call_tool("get_positions", QJsonObject{});
        QVERIFY2(res.success,
                 qPrintable("un-armed read is a handler-gate rejection, not a host denial: "
                            + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("not armed"),
                 qPrintable("reason must be the fast-arm check: " + d.value("reason").toString()));
        clear_keys();
    }

    // ── GATE: kill switch engaged (still fully fast-armed) → the host checker
    // (which gates only on the three arms) lets it THROUGH to the handler, whose
    // fast_live_gate returns a structured rejection. res.success==TRUE here. ──
    void get_positions_kill_switch_rejected_in_handler() {
        arm_fast_live();
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool("get_positions", QJsonObject{});
        QVERIFY2(res.success,
                 qPrintable("kill switch is a handler-gate rejection, not a host denial: "
                            + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        clear_keys();
    }

    // ── GATE: armed but no allowed account → handler-gate rejection. ──
    void get_positions_no_allowed_account_rejected_in_handler() {
        arm_fast_live();
        set_key("cli.allowed_account", ""); // default-deny
        auto res = rt_.call_tool("get_positions", QJsonObject{});
        QVERIFY2(res.success,
                 qPrintable("missing allowed account is a handler-gate rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("no allowed account"),
                 qPrintable("reason: " + d.value("reason").toString()));
        clear_keys();
    }

    // ════════════════════════════════════════════════════════════════════
    // Task 3: the fast-live DE-RISKING tools (cancel_order / exit_position)
    // ════════════════════════════════════════════════════════════════════

    // Most-recent audit row matching tool+decision, asserting reason is non-empty
    // (trade_audit.reason is NOT NULL — a blank reason would still insert, so we
    // guard the constraint here). Returns true if such a row exists.
    static bool has_audit(const QString& tool, const QString& decision) {
        auto r = TradeAuditRepository::instance().recent(50);
        if (!r.is_ok())
            return false;
        for (const auto& row : r.value())
            if (row.tool == tool && row.decision == decision && !row.reason.isEmpty()
                && row.mode == QLatin1String("live"))
                return true;
        return false;
    }

    // ── CONTRACT: both de-risking tools register as category "fast-live" +
    // is_destructive=true. category "fast-live" (NOT "live-trading") keeps
    // is_live_execution_tool from denying them; destructive makes the host
    // checker fire (unlike the reads). ──
    void derisk_tools_register_fast_live_destructive() {
        const auto tools = mcp::McpProvider::instance().audit_all_tools();
        int found = 0;
        for (const char* nm : {"cancel_order", "exit_position"}) {
            bool seen = false;
            for (const auto& t : tools) {
                if (t.name != QLatin1String(nm))
                    continue;
                seen = true;
                ++found;
                QCOMPARE(t.category, QStringLiteral("fast-live"));
                QVERIFY2(t.is_destructive, qPrintable(QString(nm) + " must be destructive"));
                QVERIFY2(t.has_handler, qPrintable(QString(nm) + " must have a handler"));
            }
            QVERIFY2(seen, qPrintable(QString("tool not registered: ") + nm));
        }
        QCOMPARE(found, 2);
    }

    // ── HAPPY: fully fast-armed → cancel_order routes to the broker, returns
    // status "cancelled", records the broker call + a non-empty audit row. ──
    void cancel_order_armed_cancels_and_audits() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("cancel_order", QJsonObject{{"order_id", "O1"}});
        QVERIFY2(res.success, qPrintable("armed cancel_order must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(d.value("order_id").toString(), QStringLiteral("O1"));
        QCOMPARE(FakeBroker::cancel_calls, 1);
        QCOMPARE(FakeBroker::last_cancel_id, QStringLiteral("O1"));
        QVERIFY2(has_audit("cancel_order", "cancelled"),
                 "cancel_order must write a non-empty 'cancelled' audit row");
        clear_keys();
    }

    // ── HAPPY: fully fast-armed → exit_position closes the position, status
    // "closed", records the broker close call + a non-empty audit row. ──
    void exit_position_armed_closes_and_audits() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("exit_position", QJsonObject{{"symbol", "AAPL"}});
        QVERIFY2(res.success, qPrintable("armed exit_position must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("closed"));
        QCOMPARE(d.value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(FakeBroker::close_calls, 1);
        QCOMPARE(FakeBroker::last_close_symbol, QStringLiteral("AAPL"));
        QVERIFY2(has_audit("exit_position", "closed"),
                 "exit_position must write a non-empty 'closed' audit row");
        clear_keys();
    }

    // ── GATE: NOT fast-armed (base live still armed) → DESTRUCTIVE, so the host
    // auth-checker (3-arm predicate) DENIES before the handler runs → res.success
    // is FALSE (a host denial, not a structured rejection), and the broker is
    // never touched. ──
    void cancel_order_not_fast_armed_denied_by_host() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.fast_live_armed", "false");
        auto res = rt_.call_tool("cancel_order", QJsonObject{{"order_id", "O1"}});
        QVERIFY2(!res.success, "not-fast-armed cancel_order must be DENIED by the host checker");
        QCOMPARE(FakeBroker::cancel_calls, 0);
        clear_keys();
    }

    void exit_position_not_fast_armed_denied_by_host() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.fast_live_armed", "false");
        auto res = rt_.call_tool("exit_position", QJsonObject{{"symbol", "AAPL"}});
        QVERIFY2(!res.success, "not-fast-armed exit_position must be DENIED by the host checker");
        QCOMPARE(FakeBroker::close_calls, 0);
        clear_keys();
    }

    // ── GATE: kill switch engaged (still fully fast-armed) → host checker (3
    // arms) lets it THROUGH; the handler's fast_live_gate returns a structured
    // rejection (res.success==TRUE, status "rejected", reason "kill switch
    // engaged"). The broker is never touched. ──
    void cancel_order_kill_switch_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool("cancel_order", QJsonObject{{"order_id", "O1"}});
        QVERIFY2(res.success, qPrintable("kill switch is a handler-gate rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        QCOMPARE(FakeBroker::cancel_calls, 0);
        clear_keys();
    }

    void exit_position_kill_switch_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool("exit_position", QJsonObject{{"symbol", "AAPL"}});
        QVERIFY2(res.success, qPrintable("kill switch is a handler-gate rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        QCOMPARE(FakeBroker::close_calls, 0);
        clear_keys();
    }

    // ── GATE: armed but no allowed account → host checker passes (3 arms set),
    // handler's fast_live_gate rejects (res.success==TRUE, status "rejected",
    // reason mentions "no allowed account"). ──
    void cancel_order_no_allowed_account_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.allowed_account", "");
        auto res = rt_.call_tool("cancel_order", QJsonObject{{"order_id", "O1"}});
        QVERIFY2(res.success, qPrintable("missing allowed account is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("no allowed account"),
                 qPrintable("reason: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::cancel_calls, 0);
        clear_keys();
    }

    void exit_position_no_allowed_account_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.allowed_account", "");
        auto res = rt_.call_tool("exit_position", QJsonObject{{"symbol", "AAPL"}});
        QVERIFY2(res.success, qPrintable("missing allowed account is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("no allowed account"),
                 qPrintable("reason: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::close_calls, 0);
        clear_keys();
    }

    // ════════════════════════════════════════════════════════════════════
    // Task 4: fast_submit_order — the ONE-SHOT gated LIVE order
    // ════════════════════════════════════════════════════════════════════

    // A within-caps limit order against the FakeBroker. A non-empty VALID
    // exchange is mandatory: UnifiedTrading::place_order runs OrderValidator,
    // which requires it, BEFORE routing to the broker.
    static QJsonObject submit_args(double qty) {
        return QJsonObject{{"symbol", "AAPL"},  {"side", "buy"},
                           {"quantity", qty},   {"order_type", "limit"},
                           {"limit_price", 200}, {"exchange", "NASDAQ"}};
    }

    // A within-caps plain-stop order (market-on-trigger): trigger_price set, NO
    // limit price. A valid exchange is mandatory (OrderValidator runs before the
    // broker) and OrderValidator requires stop_price>0 for SL/SL-M orders.
    static QJsonObject stop_args(double qty, double trigger) {
        return QJsonObject{{"symbol", "AAPL"}, {"side", "buy"},
                           {"quantity", qty},  {"order_type", "stop"},
                           {"trigger_price", trigger}, {"exchange", "NASDAQ"}};
    }

    // ── CONTRACT: registers category "fast-live" + is_destructive=true. The
    // destructive pairing makes the host's is_fast_live_tool checker gate it on
    // the full fast-arm predicate (unlike the non-destructive reads). ──
    void fast_submit_registers_fast_live_destructive() {
        const auto tools = mcp::McpProvider::instance().audit_all_tools();
        bool seen = false;
        for (const auto& t : tools) {
            if (t.name != QLatin1String("fast_submit_order"))
                continue;
            seen = true;
            QCOMPARE(t.category, QStringLiteral("fast-live"));
            QVERIFY2(t.is_destructive, "fast_submit_order must be destructive");
            QVERIFY2(t.has_handler, "fast_submit_order must have a handler");
        }
        QVERIFY2(seen, "fast_submit_order not registered");
    }

    // ── HAPPY: fully fast-armed + within caps → status "filled", FakeBroker
    // received the order, a live_positions row exists, and a non-empty
    // fast_submit_order/live/filled audit row is written. ──
    void fast_submit_happy_fills_records_and_audits() {
        FakeBroker::reset_derisk_counters();
        const QString acct = arm_fast_live();
        auto res = rt_.call_tool("fast_submit_order", submit_args(5));
        QVERIFY2(res.success, qPrintable("armed fast_submit_order must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(FakeBroker::place_calls, 1);
        QCOMPARE(FakeBroker::last_order.symbol, QStringLiteral("AAPL"));
        QCOMPARE(FakeBroker::last_order.quantity, 5.0);
        // The fill is recorded in the live realized-P&L ledger.
        auto open = LivePnlRepository::instance().get_open(acct, "equity", "AAPL");
        QVERIFY2(open.is_ok() && open.value().has_value(),
                 "a live_positions row must exist after a fill");
        QCOMPARE(open.value()->qty, 5.0);
        QVERIFY2(has_audit("fast_submit_order", "filled"),
                 "fast_submit_order must write a non-empty 'filled' audit row");
        clear_keys();
    }

    // ── GATE: NOT fast-armed (base live still armed) → DESTRUCTIVE, so the host
    // auth-checker (3-arm predicate) DENIES before the handler runs → res.success
    // FALSE and the broker is NEVER touched. ──
    void fast_submit_not_fast_armed_denied_by_host() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.fast_live_armed", "false");
        auto res = rt_.call_tool("fast_submit_order", submit_args(5));
        QVERIFY2(!res.success, "not-fast-armed fast_submit_order must be DENIED by the host checker");
        QCOMPARE(FakeBroker::place_calls, 0);
        clear_keys();
    }

    // ── GATE: kill switch engaged (still fully fast-armed) → host checker (3
    // arms) lets it THROUGH; the handler's fast_live_gate rejects (success==TRUE,
    // status "rejected", reason "kill switch engaged"). Broker never touched. ──
    void fast_submit_kill_switch_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool("fast_submit_order", submit_args(5));
        QVERIFY2(res.success, qPrintable("kill switch is a handler-gate rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        QCOMPARE(FakeBroker::place_calls, 0);
        clear_keys();
    }

    // ── GATE: armed but no allowed account → handler's fast_live_gate rejects. ──
    void fast_submit_no_allowed_account_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.allowed_account", "");
        auto res = rt_.call_tool("fast_submit_order", submit_args(5));
        QVERIFY2(res.success,
                 qPrintable("missing allowed account is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("no allowed account"),
                 qPrintable("reason: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::place_calls, 0);
        clear_keys();
    }

    // ── FLOOR: oversized (qty 1000 @ 200 = 200000 > the 25000 default cap) →
    // rejected at the deterministic floor, the broker is NEVER called. ──
    void fast_submit_oversized_rejected_at_floor_no_broker() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("fast_submit_order", submit_args(1000));
        QVERIFY2(res.success, qPrintable("oversized is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("max order value"),
                 qPrintable("reason must be the order-value floor: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::place_calls, 0); // floor gates BEFORE the broker
        clear_keys();
    }

    // ── DAILY-LOSS: pre-seed a realized loss over the GUI default cap (5000) →
    // rejected at the daily-loss floor, the broker is NEVER called. ──
    void fast_submit_daily_loss_rejected_no_broker() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto seed =
            LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), -6000.0);
        QVERIFY2(seed.is_ok(), "seed add_realized must succeed");
        auto res = rt_.call_tool("fast_submit_order", submit_args(5));
        QVERIFY2(res.success, qPrintable("daily-loss is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("daily loss"),
                 qPrintable("reason must be the daily-loss floor: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::place_calls, 0);
        // Undo the seed so later slots see a clean tally.
        LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), 6000.0);
        clear_keys();
    }

    // ── FIX B: a SMALL realized loss must NOT reject every subsequent order.
    // Seed -100 (today_loss 100, default cap 5000). A within-headroom order's
    // worst-case loss is its notional (5*200=1000); 100+1000 <= 5000 → ALLOWED
    // (filled). BEFORE the fix daily_loss_ok was (wrongly) fed the CAP itself
    // (5000): 100+5000 > 5000 → every order after the first cent of loss was
    // rejected. This slot is RED on the old code, GREEN on the fix. ──
    void fast_submit_small_loss_still_allows_within_cap() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto seed =
            LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), -100.0);
        QVERIFY2(seed.is_ok(), "seed add_realized must succeed");
        auto res = rt_.call_tool("fast_submit_order", submit_args(5)); // notional 1000
        QVERIFY2(res.success, qPrintable("within-cap submit must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(FakeBroker::place_calls, 1);
        // Undo the seed so later slots see a clean tally.
        LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), 100.0);
        clear_keys();
    }

    // ── FIX B: the gate STILL rejects when the running loss plus THIS order's
    // worst-case loss would breach the cap. Seed -100; a 25*200=5000 notional →
    // 100+5000 > 5000 → "daily loss limit reached". 5000 <= the 25000 default
    // order-value cap, so the order clears the value floor first and the
    // daily-loss floor is the one that fires. The broker is NEVER called. ──
    void fast_submit_loss_plus_order_over_cap_rejected() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto seed =
            LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), -100.0);
        QVERIFY2(seed.is_ok(), "seed add_realized must succeed");
        auto res = rt_.call_tool("fast_submit_order", submit_args(25)); // notional 5000
        QVERIFY2(res.success, qPrintable("daily-loss is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("daily loss"),
                 qPrintable("reason must be the daily-loss floor: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::place_calls, 0);
        LivePnlRepository::instance().add_realized(mcp::tools::today_utc(), 100.0);
        clear_keys();
    }

    // ── FIX A: a plain stop (market-on-trigger) wires trigger_price into
    // order.stop_price and fires through the broker with price 0. The risk floor
    // references the trigger level (210) as the resolved price. ──
    void fast_submit_stop_wires_trigger_price() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("fast_submit_order", stop_args(5, 210));
        QVERIFY2(res.success, qPrintable("armed stop submit must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(FakeBroker::place_calls, 1);
        QCOMPARE(FakeBroker::last_order.order_type, trading::OrderType::StopLoss);
        QCOMPARE(FakeBroker::last_order.stop_price, 210.0);
        QCOMPARE(FakeBroker::last_order.price, 0.0);
        clear_keys();
    }

    // ── FIX A: a stop_limit carries BOTH the trigger (stop_price) and the limit
    // (price). The risk floor references the limit price (205). ──
    void fast_submit_stop_limit_wires_both_prices() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool(
            "fast_submit_order",
            QJsonObject{{"symbol", "AAPL"}, {"side", "buy"},        {"quantity", 5},
                        {"order_type", "stop_limit"}, {"trigger_price", 210},
                        {"limit_price", 205}, {"exchange", "NASDAQ"}});
        QVERIFY2(res.success, qPrintable("armed stop_limit submit must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(FakeBroker::last_order.order_type, trading::OrderType::StopLossLimit);
        QCOMPARE(FakeBroker::last_order.stop_price, 210.0);
        QCOMPARE(FakeBroker::last_order.price, 205.0);
        clear_keys();
    }

    // ── FIX A: a stop WITHOUT trigger_price is rejected before the broker. ──
    void fast_submit_stop_without_trigger_rejected() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool(
            "fast_submit_order",
            QJsonObject{{"symbol", "AAPL"}, {"side", "buy"}, {"quantity", 5},
                        {"order_type", "stop"}, {"exchange", "NASDAQ"}});
        QVERIFY2(res.success,
                 qPrintable("missing trigger is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("trigger_price required"),
                 qPrintable("reason must be the trigger check: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::place_calls, 0);
        clear_keys();
    }

    // ════════════════════════════════════════════════════════════════════
    // Task 5: replace_order — the gated, risk-floored LIVE order MODIFY
    // ════════════════════════════════════════════════════════════════════

    // The new order params for a modify. No exchange needed: modify_order routes
    // straight to broker->modify_order (NO OrderValidator, unlike place_order), so
    // the only use of these params is building the UnifiedOrder for the risk floor.
    static QJsonObject replace_args(double qty, double limit_price) {
        return QJsonObject{{"order_id", "O1"}, {"symbol", "AAPL"},
                           {"side", "buy"},     {"quantity", qty},
                           {"order_type", "limit"}, {"limit_price", limit_price}};
    }

    // ── CONTRACT: registers category "fast-live" + is_destructive=true. The
    // destructive pairing makes the host's is_fast_live_tool checker gate it on the
    // full fast-arm predicate — so the not-fast-armed slot below proves real host
    // gating on a REGISTERED tool, not an unknown-tool artifact. ──
    void replace_order_registers_fast_live_destructive() {
        const auto tools = mcp::McpProvider::instance().audit_all_tools();
        bool seen = false;
        for (const auto& t : tools) {
            if (t.name != QLatin1String("replace_order"))
                continue;
            seen = true;
            QCOMPARE(t.category, QStringLiteral("fast-live"));
            QVERIFY2(t.is_destructive, "replace_order must be destructive");
            QVERIFY2(t.has_handler, "replace_order must have a handler");
        }
        QVERIFY2(seen, "replace_order not registered");
    }

    // ── HAPPY: fully fast-armed + within caps → status "replaced", FakeBroker
    // received the modify for order_id "O1", and a non-empty replace_order/live/
    // replaced audit row is written. ──
    void replace_order_happy_replaces_and_audits() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("replace_order", replace_args(5, 201));
        QVERIFY2(res.success, qPrintable("armed replace_order must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("replaced"));
        QCOMPARE(d.value("order_id").toString(), QStringLiteral("O1"));
        QCOMPARE(FakeBroker::modify_calls, 1);
        QCOMPARE(FakeBroker::last_modify_id, QStringLiteral("O1"));
        QVERIFY2(has_audit("replace_order", "replaced"),
                 "replace_order must write a non-empty 'replaced' audit row");
        clear_keys();
    }

    // ── FLOOR: oversized new params (qty 1000 @ 200 = 200000 > the 25000 default
    // cap) → rejected at the deterministic floor, modify_order is NEVER called. ──
    void replace_order_oversized_rejected_at_floor_no_broker() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool("replace_order", replace_args(1000, 200));
        QVERIFY2(res.success, qPrintable("oversized is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("max order value"),
                 qPrintable("reason must be the order-value floor: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::modify_calls, 0); // floor gates BEFORE the broker
        clear_keys();
    }

    // ── GATE: NOT fast-armed (base live still armed) → DESTRUCTIVE, so the host
    // auth-checker (3-arm predicate) DENIES before the handler runs → res.success
    // FALSE and the broker is NEVER touched. ──
    void replace_order_not_fast_armed_denied_by_host() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.fast_live_armed", "false");
        auto res = rt_.call_tool("replace_order", replace_args(5, 201));
        QVERIFY2(!res.success, "not-fast-armed replace_order must be DENIED by the host checker");
        QCOMPARE(FakeBroker::modify_calls, 0);
        clear_keys();
    }

    // ── GATE: kill switch engaged (still fully fast-armed) → host checker (3 arms)
    // lets it THROUGH; the handler's fast_live_gate rejects (success==TRUE, status
    // "rejected", reason "kill switch engaged"). Broker never touched. ──
    void replace_order_kill_switch_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.kill_switch", "true");
        auto res = rt_.call_tool("replace_order", replace_args(5, 201));
        QVERIFY2(res.success, qPrintable("kill switch is a handler-gate rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(d.value("reason").toString(), QStringLiteral("kill switch engaged"));
        QCOMPARE(FakeBroker::modify_calls, 0);
        clear_keys();
    }

    // ── GATE: armed but no allowed account → handler's fast_live_gate rejects. ──
    void replace_order_no_allowed_account_rejected_in_handler() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        set_key("cli.allowed_account", "");
        auto res = rt_.call_tool("replace_order", replace_args(5, 201));
        QVERIFY2(res.success,
                 qPrintable("missing allowed account is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("no allowed account"),
                 qPrintable("reason: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::modify_calls, 0);
        clear_keys();
    }

    // ── FIX A: replace_order with order_type "stop" wires trigger_price into the
    // broker modify mods (and the risk floor references the trigger level). ──
    void replace_order_stop_wires_trigger_price() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool(
            "replace_order",
            QJsonObject{{"order_id", "O1"}, {"symbol", "AAPL"}, {"side", "buy"},
                        {"quantity", 5}, {"order_type", "stop"}, {"trigger_price", 210}});
        QVERIFY2(res.success, qPrintable("armed stop replace must succeed: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("replaced"));
        QCOMPARE(FakeBroker::modify_calls, 1);
        QCOMPARE(FakeBroker::last_modify_id, QStringLiteral("O1"));
        QCOMPARE(FakeBroker::last_mods.value("trigger_price").toDouble(), 210.0);
        clear_keys();
    }

    // ── FIX A: replace_order stop WITHOUT trigger_price is rejected before the
    // broker (modify_order is never called). ──
    void replace_order_stop_without_trigger_rejected() {
        FakeBroker::reset_derisk_counters();
        arm_fast_live();
        auto res = rt_.call_tool(
            "replace_order",
            QJsonObject{{"order_id", "O1"}, {"symbol", "AAPL"}, {"side", "buy"},
                        {"quantity", 5}, {"order_type", "stop"}});
        QVERIFY2(res.success,
                 qPrintable("missing trigger is a handler rejection: " + res.error));
        const QJsonObject d = res.data.toObject();
        QCOMPARE(d.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(d.value("reason").toString().contains("trigger_price required"),
                 qPrintable("reason must be the trigger check: " + d.value("reason").toString()));
        QCOMPARE(FakeBroker::modify_calls, 0);
        clear_keys();
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstFastLive)
#include "tst_fast_live.moc"
