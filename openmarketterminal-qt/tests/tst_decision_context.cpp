// tst_decision_context.cpp — DecisionContext::assess (ai ctx decision-packet
// Task 2): pure-read assembly of a decision-ready packet from
// edge_decision_journal + the cost/freshness/lane machinery.
//
// DB bring-up mirrors tst_sandbox_registry.cpp's
// open_profile_database_for_test() (select the "default" profile, create its
// datadir tree, register migrations, then open the DB).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/ai_decision/DecisionContext.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::ai_decision;

namespace {

// Copies the exact DB-open incantation from tst_sandbox_registry.cpp. Do not
// invent a new bootstrap.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

} // namespace

class TstDecisionContext : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    void assess_reads_edge_and_gates() {
        // Seed one edge row: fresh, positive edge_after_cost, gate=pass.
        auto ins = Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, side, gate,"
            " market_probability, model_probability, edge_after_cost, spread_cost, fee_cost, confidence,"
            " freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("dc1"), 1000, 1000, QStringLiteral("BTC-USD"), QStringLiteral("buy"),
             QStringLiteral("pass"), 0.40, 0.55, 8.0, 2.0, 1.0, 0.8,
             QStringLiteral("{\"freshest_age_ms\":120,\"live_sources\":3}"), QStringLiteral("edge crypto-recommend")});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        const auto p = assess(QStringLiteral("BTC-USD"));
        QVERIFY(p.has_edge_signal);
        QVERIFY(qAbs(p.edge_after_cost - 8.0) < 1e-9);
        QCOMPARE(p.clears_cost, QStringLiteral("true"));         // edge_after_cost>0 && gate!=fail
        QVERIFY(qAbs(p.round_trip_cost_bps - 3.0) < 1e-9);       // spread_cost+fee_cost
        QCOMPARE(p.freshness, QStringLiteral("ok"));
        QCOMPARE(p.recommendation_hint, QStringLiteral("all gates pass"));
    }

    void assess_below_cost_and_stale_and_missing() {
        // gate=fail => clears_cost false; hint "blocked: below cost" when fresh.
        Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, edge_after_cost,"
            " spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("dc2"), 2000, 2000, QStringLiteral("ETH-USD"), QStringLiteral("fail"), -1.0,
             1.0, 1.0, QStringLiteral("{\"freshest_age_ms\":120,\"live_sources\":3}"), QStringLiteral("x")});
        const auto below = assess(QStringLiteral("ETH-USD"));
        QCOMPARE(below.clears_cost, QStringLiteral("false"));
        QCOMPARE(below.recommendation_hint, QStringLiteral("blocked: below cost"));

        // stale (DEGRADED) row => hint "blocked: stale data". freshest_age_ms
        // 9000 > 5000 => data_quality_from_freshness returns "degraded", which
        // is the ONLY freshness value that blocks (not "unknown").
        Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, edge_after_cost,"
            " freshness_json, source) VALUES (?,?,?,?,?,?,?,?)",
            {QStringLiteral("dc3"), 3000, 3000, QStringLiteral("SOL-USD"), QStringLiteral("pass"), 5.0,
             QStringLiteral("{\"freshest_age_ms\":9000,\"live_sources\":3}"), QStringLiteral("x")});
        const auto stale = assess(QStringLiteral("SOL-USD"));
        QCOMPARE(stale.freshness, QStringLiteral("degraded"));
        QCOMPARE(stale.recommendation_hint, QStringLiteral("blocked: stale data"));

        // UNKNOWN-freshness row (regression guard): freshness_json = {} carries
        // NEITHER freshest_age_ms NOR live_sources, so data_quality_from_freshness
        // returns "unknown" -- meaning "no telemetry", NOT "stale". Prediction/
        // Kalshi journal rows legitimately carry none. With edge present,
        // gate=pass, edge_after_cost>0, the hint MUST fall through to
        // "all gates pass", never "blocked: stale data" -- otherwise every
        // prediction candidate is falsely blocked.
        Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, edge_after_cost,"
            " spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("dc5"), 5000, 5000, QStringLiteral("KXBTC-USD"), QStringLiteral("pass"), 4.0,
             1.0, 0.5, QStringLiteral("{}"), QStringLiteral("kalshi auto-plan")});
        const auto unknown = assess(QStringLiteral("KXBTC-USD"));
        QVERIFY(unknown.has_edge_signal);
        QCOMPARE(unknown.freshness, QStringLiteral("unknown"));
        QCOMPARE(unknown.clears_cost, QStringLiteral("true"));
        QCOMPARE(unknown.recommendation_hint, QStringLiteral("all gates pass"));

        // no row => graceful, no edge.
        const auto none = assess(QStringLiteral("ZZZZ-USD"));
        QVERIFY(!none.has_edge_signal);
        QCOMPARE(none.recommendation_hint, QStringLiteral("no edge signal"));
    }

    // to_json emits the packet's fields, keyed by symbol/clears_cost/etc.
    void to_json_emits_packet_fields() {
        Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, edge_after_cost,"
            " spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("dc4"), 4000, 4000, QStringLiteral("DOGE-USD"), QStringLiteral("pass"), 3.0,
             1.0, 0.5, QStringLiteral("{\"freshest_age_ms\":100,\"live_sources\":3}"), QStringLiteral("x")});
        const auto p = assess(QStringLiteral("DOGE-USD"));
        const auto obj = to_json(p);
        QCOMPARE(obj.value(QStringLiteral("symbol")).toString(), QStringLiteral("DOGE-USD"));
        QVERIFY(obj.value(QStringLiteral("has_edge_signal")).toBool());
        QCOMPARE(obj.value(QStringLiteral("clears_cost")).toString(), QStringLiteral("true"));
        QCOMPARE(obj.value(QStringLiteral("recommendation_hint")).toString(), QStringLiteral("all gates pass"));
        QCOMPARE(obj.value(QStringLiteral("position_source")).toString(), QStringLiteral("ai_ledger"));
    }

    void ctx_position_aggregates_across_handlers() {
        // Two handlers, same symbol: A net +5 (buy 5), B net +3 (buy 3).
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('af1','A','AGG-USD','buy',5,100,0,0,1000,'d'),"
            "       ('af2','B','AGG-USD','buy',3,100,0,0,1001,'d')").is_ok());
        const auto pkt = ai_decision::assess(QStringLiteral("AGG-USD"));
        QCOMPARE(pkt.position_source, QStringLiteral("ai_ledger"));
        QCOMPARE(pkt.position_qty, 8.0);
        QCOMPARE(pkt.buying_power, -1.0);
    }

    void ctx_position_nets_opposing_handlers_to_zero() {
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('af3','A','NET-USD','buy',5,100,0,0,1000,'d'),"
            "       ('af4','B','NET-USD','sell',5,100,0,0,1001,'d')").is_ok());
        const auto pkt = ai_decision::assess(QStringLiteral("NET-USD"));
        QCOMPARE(pkt.position_qty, 0.0);
        QCOMPARE(pkt.position_source, QStringLiteral("ai_ledger"));
    }

    void ctx_position_flat_when_no_fills() {
        const auto pkt = ai_decision::assess(QStringLiteral("NONE-USD"));
        QCOMPARE(pkt.position_qty, 0.0);
        QCOMPARE(pkt.position_source, QStringLiteral("ai_ledger"));
    }
};
QTEST_GUILESS_MAIN(TstDecisionContext)
#include "tst_decision_context.moc"
