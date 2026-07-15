// tst_sandbox_executor.cpp — PaperExecutor::run_cycle (Task 5): candidate
// selection/opening, pending_fill -> open|unfilled, and open -> closed.
//
// DB bring-up mirrors tst_sandbox_registry.cpp / tst_sandbox_schema.cpp's
// initTestCase(): select the "default" profile, create its datadir tree,
// register migrations, then open the DB (which runs them). The DB (and any
// strategies/journal rows registered) persists across test slots within this
// binary, so every test below uses its OWN unique placeholder symbol,
// journal source, and strategy kind string to stay fully isolated from every
// other slot -- ticks/decisions fixtures live under a fresh QTemporaryDir
// per test, so there is no file-level collision either.

#include <QtTest>
#include <QTemporaryDir>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/sandbox/PaperExecutor.h"
#include "services/sandbox/SandboxRegistry.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services::sandbox;

namespace {

// Copies the exact DB-open incantation from tst_sandbox_registry.cpp /
// tst_data_services.cpp. Do not invent a new bootstrap.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

// A handful of positions below (tests a/c/g) are deliberately left
// pending_fill/open at the end of their own test (never advanced or
// closed there) so a LATER test's own now_ms could otherwise spuriously
// mark them unfilled/expired -- step2/step3 scan sandbox_position GLOBALLY,
// not scoped to one strategy under test, matching production behavior. A
// horizon far beyond anything any other test's now_ms reaches keeps those
// leftovers inert for the rest of this binary's run.
constexpr qint64 kFarHorizonSec = 100000000;

QString journal_json(const QJsonObject& o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Inserts one edge_decision_journal fixture row. Only the columns run_cycle's
// candidate queries actually read are given explicit values; every other
// NOT NULL column falls back to its schema DEFAULT.
void insert_journal_row(const QString& id, const QString& source, const QString& symbol, const QString& side,
                        const QString& call, const QString& gate, double confidence, qint64 created_at,
                        const QString& horizon, const QJsonObject& features, const QJsonObject& freshness,
                        double market_probability = 0.0, const QString& market_id = QString(),
                        int seconds_left = -1, double fee_cost = 0.0, double edge_after_cost = 0.0) {
    auto r = Database::instance().execute(
        "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, horizon, side, call, gate,"
        " market_id, market_probability, model_probability, confidence, seconds_left, fee_cost, edge_after_cost,"
        " freshness_json, features_json, source)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, created_at, created_at, symbol, horizon, side, call, gate,
         market_id.isEmpty() ? id : market_id, market_probability, market_probability, confidence, seconds_left,
         fee_cost, edge_after_cost,
         journal_json(freshness), journal_json(features), source});
    QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
}

struct PositionRow {
    bool found = false;
    QString position_id, strategy_id, decision_id, symbol, side, state, close_reason, data_quality;
    bool hypothetical = false;
    double qty = 0, limit_price = 0, entry_fee = 0, exit_fee = 0, notional_usd = 0;
    bool has_target = false, has_stop = false;
    double target_price = 0, stop_price = 0;
    bool has_realized_pnl = false;
    double realized_pnl = 0;
    qint64 expires_at = 0, created_at = 0;
    bool has_opened_at = false, has_closed_at = false;
    qint64 opened_at = 0, closed_at = 0;
};

PositionRow fetch_position(const QString& decision_id) {
    PositionRow row;
    auto r = Database::instance().execute(
        "SELECT position_id, strategy_id, decision_id, symbol, side, hypothetical, qty, limit_price,"
        " target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee, exit_fee,"
        " realized_pnl, close_reason, data_quality, notional_usd, created_at"
        " FROM sandbox_position WHERE decision_id = ?",
        {decision_id});
    if (r.is_err() || !r.value().next())
        return row;
    auto& q = r.value();
    row.found = true;
    row.position_id = q.value(0).toString();
    row.strategy_id = q.value(1).toString();
    row.decision_id = q.value(2).toString();
    row.symbol = q.value(3).toString();
    row.side = q.value(4).toString();
    row.hypothetical = q.value(5).toInt() != 0;
    row.qty = q.value(6).toDouble();
    row.limit_price = q.value(7).toDouble();
    row.has_target = !q.value(8).isNull();
    row.target_price = q.value(8).toDouble();
    row.has_stop = !q.value(9).isNull();
    row.stop_price = q.value(9).toDouble();
    row.expires_at = q.value(10).toLongLong();
    row.state = q.value(11).toString();
    row.has_opened_at = !q.value(12).isNull();
    row.opened_at = q.value(12).toLongLong();
    row.has_closed_at = !q.value(13).isNull();
    row.closed_at = q.value(13).toLongLong();
    row.entry_fee = q.value(14).toDouble();
    row.exit_fee = q.value(15).toDouble();
    row.has_realized_pnl = !q.value(16).isNull();
    row.realized_pnl = q.value(16).toDouble();
    row.close_reason = q.value(17).toString();
    row.data_quality = q.value(18).toString();
    row.notional_usd = q.value(19).toDouble();
    row.created_at = q.value(20).toLongLong();
    return row;
}

int count_positions(const QString& decision_id) {
    auto r = Database::instance().execute("SELECT count(*) FROM sandbox_position WHERE decision_id = ?",
                                           {decision_id});
    if (r.is_err() || !r.value().next())
        return -1;
    return r.value().value(0).toInt();
}

int count_positions(const QString& strategy_id, const QString& decision_id) {
    auto r = Database::instance().execute(
        "SELECT count(*) FROM sandbox_position WHERE strategy_id = ? AND decision_id = ?",
        {strategy_id, decision_id});
    if (r.is_err() || !r.value().next())
        return -1;
    return r.value().value(0).toInt();
}

bool has_fill(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT count(*) FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    return r.is_ok() && r.value().next() && r.value().value(0).toInt() > 0;
}

double fill_fee(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT fee FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    if (r.is_err() || !r.value().next())
        return -1.0;
    return r.value().value(0).toDouble();
}

double fill_price(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT price FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    if (r.is_err() || !r.value().next())
        return -1.0;
    return r.value().value(0).toDouble();
}

QString fill_note(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT note FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    if (r.is_err() || !r.value().next())
        return QStringLiteral("<no fill row>");
    return r.value().value(0).toString();
}

// One scalp_ticks.jsonl / scalp_decisions.jsonl line writer -- same shape as
// tst_sandbox_ticktail.cpp's tick_line and ServeCommand's decision object.
QString tick_line(const QString& symbol, double price, qint64 ts_ms) {
    return QStringLiteral(R"({"symbol":"%1","price":%2,"best_bid":%2,"best_ask":%2,"received_ts_ms":"%3"})")
        .arg(symbol)
        .arg(price)
        .arg(ts_ms);
}

QString maker_tick_line(const QString& symbol, const QString& venue, double price, qint64 ts_ms) {
    return QStringLiteral(
               R"({"symbol":"%1","venue":"%2","price":%3,"best_bid":%3,"best_ask":%3,"received_ts_ms":"%4"})")
        .arg(symbol, venue).arg(price).arg(ts_ms);
}

void write_lines(const QString& path, const QStringList& lines) {
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qUtf8Printable(path));
    for (const QString& line : lines) {
        f.write(line.toUtf8());
        f.write("\n");
    }
}

} // namespace

class TstSandboxExecutor : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    // (a) spot candidate -> position opened pending_fill with correct
    // limit/target/stop/expiry/qty.
    void spot_candidate_opens_pending_fill_with_correct_math() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_a"), QStringLiteral("XAA-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-a"}, {"min_confidence", 0.5},
                        {"min_horizon_sec", 60}, {"max_age_sec", 3600}, {"entry_offset_bps", 10.0},
                        {"target_move_pct", 5.0}, {"stop_move_pct", 2.0}, {"horizon_sec", kFarHorizonSec}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 1000000;
        insert_journal_row(QStringLiteral("dec-a1"), QStringLiteral("journal-a"), QStringLiteral("XAA-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-a1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("pending_fill"));
        QCOMPARE(row.symbol, QStringLiteral("XAA-USD"));
        QCOMPARE(row.side, QStringLiteral("buy"));
        QVERIFY(!row.hypothetical);
        QVERIFY(qAbs(row.limit_price - 99.9) < 1e-9); // 100 * (1 - 10/10000)
        QVERIFY(row.has_target);
        QVERIFY(qAbs(row.target_price - 99.9 * 1.05) < 1e-6);
        QVERIFY(row.has_stop);
        QVERIFY(qAbs(row.stop_price - 99.9 * 0.98) < 1e-6);
        QCOMPARE(row.expires_at, t0 + kFarHorizonSec * 1000);
        QVERIFY(qAbs(row.qty - (100.0 / 99.9)) < 1e-9);
        QCOMPARE(row.data_quality, QStringLiteral("ok"));
        QVERIFY(has_fill(row.position_id, QStringLiteral("open")));
    }

    // (b) same candidate on a second cycle -> skipped (SQL-level anti-join
    // against sandbox_position.decision_id): no second position, opened==0.
    void same_candidate_second_cycle_is_not_reopened() {
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        const qint64 t0 = 1000000; // same journal row as test (a); still present, not consumed
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 2000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 0);
        QCOMPARE(count_positions(QStringLiteral("dec-a1")), 1);
    }

    // (c) ticks trading through the limit -> pending_fill becomes open, with
    // an entry fee (maker bps) and a 'fill' sandbox_fill row.
    void ticks_through_limit_fill_position() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_c"), QStringLiteral("XCC-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-c"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", kFarHorizonSec}, {"maker_bps", 40.0},
                        {"taker_bps", 60.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 2000000;
        insert_journal_row(QStringLiteral("dec-c1"), QStringLiteral("journal-c"), QStringLiteral("XCC-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto opened = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(opened.is_ok(), opened.is_err() ? opened.error().c_str() : "");
        QCOMPARE(opened.value().opened, 1);

        // limit_price == 100 (no entry_offset_bps configured). A tick at
        // 99.5 after creation qualifies a buy fill.
        write_lines(daemon.filePath("scalp_ticks.jsonl"), {tick_line(QStringLiteral("XCC-USD"), 99.5, t0 + 2000)});

        auto filled = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 3000);
        QVERIFY2(filled.is_ok(), filled.is_err() ? filled.error().c_str() : "");
        QCOMPARE(filled.value().filled, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-c1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
        QVERIFY(row.has_opened_at);
        QCOMPARE(row.opened_at, t0 + 2000);
        QVERIFY(qAbs(row.entry_fee - (100.0 * 40.0 / 10000.0)) < 1e-9);
        QVERIFY(has_fill(row.position_id, QStringLiteral("fill")));
        QVERIFY(qAbs(fill_fee(row.position_id, QStringLiteral("fill")) - row.entry_fee) < 1e-9);
    }

    // (d) ticks never reach the limit before the entry deadline -> unfilled.
    void ticks_never_reach_limit_marks_unfilled() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_d"), QStringLiteral("XDD-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-d"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", 100}}); // short horizon -> short entry deadline
        QVERIFY(strat.is_ok());

        const qint64 t0 = 3000000;
        insert_journal_row(QStringLiteral("dec-d1"), QStringLiteral("journal-d"), QStringLiteral("XDD-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto opened = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(opened.is_ok(), opened.is_err() ? opened.error().c_str() : "");
        QCOMPARE(opened.value().opened, 1);

        // expires_at == t0 + 100000. A tick that never qualifies a buy fill
        // (always above the 100 limit).
        write_lines(daemon.filePath("scalp_ticks.jsonl"), {tick_line(QStringLiteral("XDD-USD"), 101.0, t0 + 5000)});

        auto after_expiry = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 100000);
        QVERIFY2(after_expiry.is_ok(), after_expiry.is_err() ? after_expiry.error().c_str() : "");
        QCOMPARE(after_expiry.value().unfilled, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-d1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("unfilled"));
        QCOMPARE(row.close_reason, QStringLiteral("unfilled"));
        QVERIFY(has_fill(row.position_id, QStringLiteral("unfilled")));
    }

    // (e) an already-open position plus a target-crossing tick -> closed,
    // realized_pnl matches a hand computation, close_reason=='target'.
    // Fixture inserted directly (advisor-recommended isolation) rather than
    // driven through the full open->fill flow.
    void open_position_target_cross_closes_with_correct_pnl() {
        auto strat = register_strategy(QStringLiteral("test_open_e"), QStringLiteral("XEE-USD"),
                                       QJsonObject{{"notional_usd", 100.0}, {"maker_bps", 40.0}, {"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());
        const QString strategy_id = strat.value();

        const qint64 t0 = 4000000;
        const qint64 expires_at = t0 + 100000;
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-e1"), strategy_id, QStringLiteral("dec-e1"), QStringLiteral("XEE-USD"),
             QStringLiteral("long"), 1.0, 100.0, 105.0, 97.0, expires_at, QStringLiteral("open"), t0, 0.4,
             QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        write_lines(daemon.filePath("scalp_ticks.jsonl"), {tick_line(QStringLiteral("XEE-USD"), 106.0, t0 + 500)});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().closed, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-e1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("target"));
        QVERIFY(row.has_closed_at);
        QCOMPARE(row.closed_at, t0 + 500);
        QVERIFY(qAbs(row.exit_fee - (100.0 * 50.0 / 10000.0)) < 1e-9); // 0.5
        QVERIFY(row.has_realized_pnl);
        // (106 - 100) * 1.0 - 0.4 (entry_fee) - 0.5 (exit_fee) = 5.1
        QVERIFY(qAbs(row.realized_pnl - 5.1) < 1e-9);
        QVERIFY(has_fill(row.position_id, QStringLiteral("target")));
    }

    // An honest taker candidate executes immediately at the adverse
    // spread/slippage-adjusted price. It must never masquerade as a resting
    // limit and wait for a later tick to "fill" it.
    void honest_taker_candidate_opens_immediately_end_to_end() {
        auto strat = register_strategy(
            QStringLiteral("test_honest_entry"), QStringLiteral("XHE-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-he"},
                        {"liquidity", "taker"}, {"target_bps", 150.0}, {"stop_bps", 75.0},
                        {"horizon_sec", kFarHorizonSec},
                        {"maker_bps", 40.0}, {"taker_bps", 50.0}, {"half_spread_bps", 2.0},
                        {"slippage_bps", 1.0}, {"maker_fill_through_bps", 0.0}, {"paper_only", true}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 5000000;
        insert_journal_row(QStringLiteral("dec-he1"), QStringLiteral("journal-he"), QStringLiteral("XHE-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"),
                           0.9, t0, QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}}, 0.9);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 400);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);
        QCOMPARE(cycle.value().filled, 0);
        const PositionRow row = fetch_position(QStringLiteral("dec-he1"));
        QCOMPARE(row.state, QStringLiteral("open"));
        QCOMPARE(row.opened_at, t0 + 400);
        QVERIFY(qAbs(row.limit_price - 100.03) < 1e-9);
        QVERIFY(qAbs(row.qty - (100.0 / 100.03)) < 1e-9);
        QVERIFY(qAbs(row.target_price - 101.5) < 1e-9); // bracket anchored to raw reference
        QVERIFY(qAbs(row.entry_fee - 0.5) < 1e-9);
        QVERIFY(qAbs(fill_price(row.position_id, QStringLiteral("open")) - 100.03) < 1e-9);
    }

    // The same signal is admitted independently by each lane's economics.
    // Here it clears the cheap maker bracket but has negative EV after the
    // taker lane's wider execution cost, so only the maker book may open.
    void weak_signal_rejected_by_expensive_lane_end_to_end() {
        const QJsonObject common{{"notional_usd", 100.0}, {"journal_source", "journal-lane-ev"},
                                 {"target_bps", 100.0}, {"stop_bps", 50.0},
                                 {"horizon_sec", kFarHorizonSec}, {"half_spread_bps", 3.0},
                                 {"slippage_bps", 0.0}, {"maker_bps", 10.0}, {"taker_bps", 37.0},
                                 {"maker_fill_through_bps", 2.0}, {"paper_only", true}};
        QJsonObject maker_params = common;
        maker_params.insert(QStringLiteral("liquidity"), QStringLiteral("maker"));
        QJsonObject taker_params = common;
        taker_params.insert(QStringLiteral("liquidity"), QStringLiteral("taker"));
        auto maker = register_strategy(QStringLiteral("test_lane_ev_maker"), QStringLiteral("XEV-USD"), maker_params);
        auto taker = register_strategy(QStringLiteral("test_lane_ev_taker"), QStringLiteral("XEV-USD"), taker_params);
        QVERIFY(maker.is_ok());
        QVERIFY(taker.is_ok());

        const qint64 t0 = 5500000;
        insert_journal_row(QStringLiteral("dec-lane-ev"), QStringLiteral("journal-lane-ev"),
                           QStringLiteral("XEV-USD"), QStringLiteral("buy"),
                           QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.8, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}}, QJsonObject{}, 0.8);
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 100);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");

        QCOMPARE(count_positions(maker.value(), QStringLiteral("dec-lane-ev")), 1);
        QCOMPARE(count_positions(taker.value(), QStringLiteral("dec-lane-ev")), 0);
    }

    // End-to-end honesty (P1): an honest TAKER lane's realized PnL must charge
    // the half-spread + slippage crossing on BOTH legs (executor previously
    // booked raw prices). 3 bps adverse per leg on a 100->106 winner.
    void honest_taker_lane_books_spread_adjusted_pnl_end_to_end() {
        auto strat = register_strategy(
            QStringLiteral("test_honest_exit"), QStringLiteral("XHX-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"liquidity", "taker"}, {"maker_bps", 40.0},
                        {"taker_bps", 50.0}, {"half_spread_bps", 2.0}, {"slippage_bps", 1.0},
                        {"maker_fill_through_bps", 0.0}, {"paper_only", true}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 6000000;
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-hx1"), strat.value(), QStringLiteral("dec-hx1"), QStringLiteral("XHX-USD"),
             QStringLiteral("long"), 1.0, 100.03, 105.0, 97.0, t0 + 100000, QStringLiteral("open"), t0, 0.5,
             QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        write_lines(daemon.filePath("scalp_ticks.jsonl"), {tick_line(QStringLiteral("XHX-USD"), 106.0, t0 + 500)});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        const PositionRow row = fetch_position(QStringLiteral("dec-hx1"));
        QCOMPARE(row.state, QStringLiteral("closed"));
        QVERIFY(row.has_realized_pnl);
        // entry 100 -> 100.03 (paid up 3bps), exit 106 -> 105.9682 (received down).
        // gross 5.9382 - 0.5 entry - 0.5 exit = 4.9382 (raw would be 5.0).
        QVERIFY(qAbs(row.realized_pnl - 4.9382) < 1e-6);
    }

    // A maker target is a resting exit order. A touch is not evidence that
    // our queue position filled; price must trade through the configured
    // margin, and the fill is booked at the target limit (no gap windfall).
    void honest_maker_exit_requires_trade_through_end_to_end() {
        auto strat = register_strategy(
            QStringLiteral("test_honest_maker_exit"), QStringLiteral("XME-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"liquidity", "maker"}, {"maker_bps", 10.0},
                        {"taker_bps", 40.0}, {"half_spread_bps", 2.0}, {"slippage_bps", 1.0},
                        {"maker_fill_through_bps", 5.0}, {"paper_only", true}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 6500000;
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-me1"), strat.value(), QStringLiteral("dec-me1"), QStringLiteral("XME-USD"),
             QStringLiteral("long"), 1.0, 100.0, 105.0, 97.0, t0 + 100000,
             QStringLiteral("open"), t0, 0.1, QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        write_lines(daemon.filePath("scalp_ticks.jsonl"),
                    {tick_line(QStringLiteral("XME-USD"), 105.0, t0 + 100),
                     tick_line(QStringLiteral("XME-USD"), 105.04, t0 + 200)});
        auto touch_cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 300);
        QVERIFY2(touch_cycle.is_ok(), touch_cycle.is_err() ? touch_cycle.error().c_str() : "");
        QCOMPARE(fetch_position(QStringLiteral("dec-me1")).state, QStringLiteral("open"));

        write_lines(daemon.filePath("scalp_ticks.jsonl"),
                    {tick_line(QStringLiteral("XME-USD"), 105.10, t0 + 400)});
        auto through_cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 500);
        QVERIFY2(through_cycle.is_ok(), through_cycle.is_err() ? through_cycle.error().c_str() : "");
        const PositionRow row = fetch_position(QStringLiteral("dec-me1"));
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("target"));
        QVERIFY(qAbs(fill_price(row.position_id, QStringLiteral("target")) - 105.0) < 1e-9);
        QVERIFY(qAbs(row.exit_fee - 0.1) < 1e-9);
        QVERIFY(qAbs(row.realized_pnl - 4.8) < 1e-9);
    }

    // (f) stop and expiry variants of the same open->closed transition.
    void open_position_stop_and_expiry_variants() {
        auto strat = register_strategy(QStringLiteral("test_open_f"), QStringLiteral("XFF-USD"),
                                       QJsonObject{{"notional_usd", 100.0}, {"maker_bps", 40.0}, {"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());
        const QString strategy_id = strat.value();

        // Stop variant: long entry 100, target 105, stop 97; tick 96 crosses
        // the stop.
        const qint64 t0 = 5000000;
        const qint64 expires_at_stop = t0 + 100000;
        auto ins1 = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-f-stop"), strategy_id, QStringLiteral("dec-f-stop"), QStringLiteral("XFF-USD"),
             QStringLiteral("long"), 1.0, 100.0, 105.0, 97.0, expires_at_stop, QStringLiteral("open"), t0, 0.4,
             QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins1.is_ok(), ins1.is_err() ? ins1.error().c_str() : "");

        QTemporaryDir daemon_stop;
        QVERIFY(daemon_stop.isValid());
        write_lines(daemon_stop.filePath("scalp_ticks.jsonl"),
                   {tick_line(QStringLiteral("XFF-USD"), 96.0, t0 + 500)});
        auto stop_cycle = run_cycle(QStringLiteral("default"), daemon_stop.path(), t0 + 1000);
        QVERIFY2(stop_cycle.is_ok(), stop_cycle.is_err() ? stop_cycle.error().c_str() : "");
        QCOMPARE(stop_cycle.value().closed, 1);

        const PositionRow stop_row = fetch_position(QStringLiteral("dec-f-stop"));
        QVERIFY(stop_row.found);
        QCOMPARE(stop_row.state, QStringLiteral("closed"));
        QCOMPARE(stop_row.close_reason, QStringLiteral("stop"));
        QVERIFY(has_fill(stop_row.position_id, QStringLiteral("stop")));
        // (96 - 100) * 1.0 - 0.4 - 0.5 = -4.9
        QVERIFY(qAbs(stop_row.realized_pnl - (-4.9)) < 1e-9);

        // Expiry variant: long entry 100, target 105, stop 97; no tick ever
        // crosses either bound, now_ms reaches expires_at.
        const qint64 t1 = 6000000;
        const qint64 expires_at_exp = t1 + 50000;
        auto ins2 = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-f-expiry"), strategy_id, QStringLiteral("dec-f-expiry"), QStringLiteral("XGG-USD"),
             QStringLiteral("long"), 1.0, 100.0, 105.0, 97.0, expires_at_exp, QStringLiteral("open"), t1, 0.4,
             QStringLiteral("ok"), 100.0, t1});
        QVERIFY2(ins2.is_ok(), ins2.is_err() ? ins2.error().c_str() : "");

        QTemporaryDir daemon_expiry;
        QVERIFY(daemon_expiry.isValid());
        write_lines(daemon_expiry.filePath("scalp_ticks.jsonl"),
                   {tick_line(QStringLiteral("XGG-USD"), 101.0, t1 + 1000)});
        auto expiry_cycle = run_cycle(QStringLiteral("default"), daemon_expiry.path(), expires_at_exp);
        QVERIFY2(expiry_cycle.is_ok(), expiry_cycle.is_err() ? expiry_cycle.error().c_str() : "");
        QCOMPARE(expiry_cycle.value().closed, 1);

        const PositionRow expiry_row = fetch_position(QStringLiteral("dec-f-expiry"));
        QVERIFY(expiry_row.found);
        QCOMPARE(expiry_row.state, QStringLiteral("closed"));
        QCOMPARE(expiry_row.close_reason, QStringLiteral("expiry"));
        QVERIFY(expiry_row.has_closed_at);
        QCOMPARE(expiry_row.closed_at, expires_at_exp);
        QVERIFY(has_fill(expiry_row.position_id, QStringLiteral("expiry")));
        // Exit at the last pre-expiry tick's price (101):
        // (101 - 100) * 1.0 - 0.4 - 0.5 = 0.1
        QVERIFY(qAbs(expiry_row.realized_pnl - 0.1) < 1e-9);
    }

    // (g) degraded freshness_json (live_sources < 2) -> data_quality ==
    // 'degraded' on the newly-opened position.
    void degraded_freshness_marks_position_degraded() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_g"), QStringLiteral("XHH-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-g"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", kFarHorizonSec}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 7000000;
        insert_journal_row(QStringLiteral("dec-g1"), QStringLiteral("journal-g"), QStringLiteral("XHH-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 1}}); // live_sources < 2

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-g1"));
        QVERIFY(row.found);
        QCOMPARE(row.data_quality, QStringLiteral("degraded"));
    }

    // (h) a paused strategy contributes no positions, even with an otherwise
    // qualifying fresh candidate sitting in the journal.
    void paused_strategy_opens_nothing() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_h"), QStringLiteral("XII-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-h"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", 3600}});
        QVERIFY(strat.is_ok());
        auto paused = set_status(strat.value(), QStringLiteral("paused"));
        QVERIFY(paused.is_ok());

        const qint64 t0 = 8000000;
        insert_journal_row(QStringLiteral("dec-h1"), QStringLiteral("journal-h"), QStringLiteral("XII-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(count_positions(QStringLiteral("dec-h1")), 0);
    }

    // Extra (beyond the binding (a)-(h) list): a prediction book (params
    // prediction:true, e.g. btc5m/kalshi) opens directly at 'open' priced at
    // the journal row's market_probability, side "yes", entry fee at taker
    // (recorded on the 'open' fill row -- review fix 6), no target/stop --
    // and is NOT advanced by step 3 (Resolver-owned). Prediction rows carry
    // no freshness telemetry, so data_quality is 'unknown' (review fix 4;
    // see PaperExecutor.h's data-quality contract).
    void prediction_candidate_opens_directly_at_market_probability() {
        auto strat = register_strategy(QStringLiteral("test_pred_i"), QStringLiteral("XPP-USD"),
                                       QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-i"},
                                                   {"prediction", true}, {"taker_bps", 60.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 9000000;
        // Empty freshness_json -- the realistic btc5m/kalshi shape (neither
        // freshest_age_ms nor live_sources).
        insert_journal_row(QStringLiteral("dec-i1"), QStringLiteral("journal-i"), QStringLiteral("XPP-USD"),
                           QStringLiteral(""), QStringLiteral(""), QStringLiteral("pass"), 0.0, t0,
                           QStringLiteral(""), QJsonObject{}, QJsonObject{},
                           /*market_probability=*/0.65);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-i1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
        QCOMPARE(row.side, QStringLiteral("yes"));
        QVERIFY(!row.hypothetical);
        QVERIFY(qAbs(row.limit_price - 0.65) < 1e-9);
        QVERIFY(qAbs(row.qty - (100.0 / 0.65)) < 1e-9);
        QVERIFY(!row.has_target);
        QVERIFY(!row.has_stop);
        QVERIFY(qAbs(row.entry_fee - (100.0 * 60.0 / 10000.0)) < 1e-9);
        // Fee bookkeeping symmetry (review fix 6): the taker entry fee paid
        // at this immediate synthetic open is recorded on the 'open'
        // sandbox_fill row itself, not left at 0.
        QVERIFY(has_fill(row.position_id, QStringLiteral("open")));
        QVERIFY(qAbs(fill_fee(row.position_id, QStringLiteral("open")) - row.entry_fee) < 1e-9);
        // data_quality three-way rule (review fix 4): a row with NO
        // freshness telemetry at all is 'unknown' -- absence of signal is
        // not evidence of degradation (prediction rows would otherwise be
        // permanently 'degraded').
        QCOMPARE(row.data_quality, QStringLiteral("unknown"));

        // Not Resolver-owned-excluded from step 3: a ticks file crossing any
        // price for this symbol must NOT close it (side 'yes' is filtered
        // out of advance_open_positions's query).
        write_lines(daemon.filePath("scalp_ticks.jsonl"), {tick_line(QStringLiteral("XPP-USD"), 999.0, t0 + 2000)});
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 3000);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        const PositionRow still_open = fetch_position(QStringLiteral("dec-i1"));
        QCOMPARE(still_open.state, QStringLiteral("open"));
    }

    // Extra (beyond the binding (a)-(h) list): scalp candidate scan over the
    // reimplemented scalp_decisions.jsonl contract (the promoted
    // TickTail::read_tail_with_prev + verdict/action/ts_ms/reference_price
    // parsing is otherwise untested new code).
    void scalp_candidate_from_decisions_jsonl_opens_position() {
        auto strat = register_strategy(
            QStringLiteral("scalp"), QStringLiteral("XSS-USD"),
            QJsonObject{{"notional_usd", 50.0}, {"venue", "kraken_pro"},
                        {"max_age_sec", 15}, {"entry_offset_bps", 1.0},
                        {"target_bps", 25.0}, {"stop_bps", 15.0}, {"horizon_sec", 900}});
        QVERIFY(strat.is_ok());
        auto wrong_venue = register_strategy(
            QStringLiteral("scalp"), QStringLiteral("XSS-USD"),
            QJsonObject{{"notional_usd", 50.0}, {"venue", "coinbase_advanced"},
                        {"max_age_sec", 15}, {"entry_offset_bps", 1.0},
                        {"target_bps", 25.0}, {"stop_bps", 15.0}, {"horizon_sec", 900}});
        QVERIFY(wrong_venue.is_ok());

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const qint64 ts_ms = now_ms - 2000; // within the 15s max_age_sec window
        const QString decision = QStringLiteral(
                                     R"({"symbol":"XSS-USD","verdict":"PAPER TRADE CANDIDATE",)"
                                     R"("action":"PAPER_LIMIT_BUY_ONLY","ts_ms":"%1","reference_price":200.0,)"
                                     R"("venue":"kraken_pro","freshest_age_ms":50,"live_sources":3})")
                                     .arg(ts_ms);
        write_lines(daemon.filePath("scalp_decisions.jsonl"), {decision});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), now_ms);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const QString decision_id = QStringLiteral("XSS-USD|%1").arg(ts_ms);
        const PositionRow row = fetch_position(decision_id);
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("pending_fill"));
        QVERIFY(qAbs(row.limit_price - 200.0 * (1.0 - 1.0 / 10000.0)) < 1e-6);
        QCOMPARE(row.expires_at, ts_ms + 900 * 1000);
        QCOMPARE(row.data_quality, QStringLiteral("ok"));
        auto wrong_count = Database::instance().execute(
            "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id=?", {wrong_venue.value()});
        QVERIFY(wrong_count.is_ok());
        QVERIFY(wrong_count.value().next());
        QCOMPARE(wrong_count.value().value(0).toInt(), 0);

        // Same fixture, second cycle -> scalp's soft pre-check dedup (not
        // the neuter-verified SQL anti-join, which is spot-only) means no
        // second position and no error.
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), now_ms + 100);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().opened, 0);
        QCOMPARE(count_positions(decision_id), 1);
    }

    // Honest taker scalp lanes consume the daemon JSONL directly, so pin
    // that production-shaped path separately from the journal-backed swing
    // test. A qualifying candidate opens immediately at the adverse taker
    // price and records the taker fee; it must never linger pending_fill.
    void honest_taker_scalp_candidate_opens_immediately_end_to_end() {
        auto strat = register_strategy(
            QStringLiteral("scalp"), QStringLiteral("XST-USD"),
            QJsonObject{{"notional_usd", 50.0}, {"venue", "test_taker"},
                        {"liquidity", "taker"}, {"max_age_sec", 15},
                        {"entry_offset_bps", 0.0}, {"target_bps", 40.0},
                        {"stop_bps", 20.0}, {"horizon_sec", 900},
                        {"maker_bps", 5.0}, {"taker_bps", 20.0},
                        {"half_spread_bps", 3.0}, {"slippage_bps", 2.0}});
        QVERIFY(strat.is_ok());

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const qint64 ts_ms = now_ms - 1000;
        const QString decision = QStringLiteral(
                                     R"({"symbol":"XST-USD","verdict":"PAPER TRADE CANDIDATE",)"
                                     R"("action":"PAPER_LIMIT_BUY_ONLY","ts_ms":"%1","reference_price":100.0,)"
                                     R"("venue":"test_taker","expected_capture_bps":75.0,)"
                                     R"("freshest_age_ms":20,"live_sources":3})")
                                     .arg(ts_ms);
        write_lines(daemon.filePath("scalp_decisions.jsonl"), {decision});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), now_ms);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("XST-USD|%1").arg(ts_ms));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
        QVERIFY(row.has_opened_at);
        QCOMPARE(row.opened_at, now_ms);
        // 3bps half-spread + 2bps slippage, adverse to a buy.
        QVERIFY(qAbs(row.limit_price - 100.05) < 1e-9);
        QVERIFY(qAbs(row.entry_fee - 0.10) < 1e-9);
        QVERIFY(qAbs(fill_fee(row.position_id, QStringLiteral("open")) - 0.10) < 1e-9);
        QVERIFY(row.has_target);
        QVERIFY(qAbs(row.target_price - 100.40) < 1e-9);
        QCOMPARE(row.data_quality, QStringLiteral("ok"));
    }

    // Regression (review fix 1, RED without the NULL-pnl branch): an open
    // position that expires with NO pre-expiry tick (only post-expiry
    // prints in the file -- check_exit's documented price-0 expiry
    // sentinel) must NOT book a fake +-notional realized_pnl against price
    // 0. Contract: closed, close_reason 'expiry', realized_pnl NULL,
    // exit_fee 0, data_quality forced 'degraded', and a sandbox_fill row
    // kind 'expiry' with the data-gap note.
    void data_gap_expiry_closes_with_null_pnl() {
        auto strat = register_strategy(QStringLiteral("test_gap_j"), QStringLiteral("XJJ-USD"),
                                       QJsonObject{{"notional_usd", 100.0}, {"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 10000000;
        const qint64 expires_at = t0 + 50000;
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-j1"), strat.value(), QStringLiteral("dec-j1"), QStringLiteral("XJJ-USD"),
             QStringLiteral("long"), 1.0, 100.0, 105.0, 97.0, expires_at, QStringLiteral("open"), t0, 0.4,
             QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        // The ONLY tick is post-expiry (and target-crossing, which must not
        // matter -- the position died at expires_at).
        write_lines(daemon.filePath("scalp_ticks.jsonl"),
                   {tick_line(QStringLiteral("XJJ-USD"), 106.0, expires_at + 5000)});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), expires_at + 10000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().closed, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-j1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("expiry"));
        QVERIFY2(!row.has_realized_pnl, "data-gap close must leave realized_pnl NULL, never +-notional");
        QCOMPARE(row.exit_fee, 0.0);
        QCOMPARE(row.data_quality, QStringLiteral("degraded"));
        QVERIFY(row.has_closed_at);
        QCOMPARE(row.closed_at, expires_at);
        QVERIFY(has_fill(row.position_id, QStringLiteral("expiry")));
        QCOMPARE(fill_note(row.position_id, QStringLiteral("expiry")),
                 QStringLiteral("data gap: no pre-expiry tick"));
        QCOMPARE(fill_fee(row.position_id, QStringLiteral("expiry")), 0.0);
    }

    // Regression (review fix 2): an open position with NULL target AND
    // stop must still EXPIRE (the old guard skipped it forever -- a
    // zombie). NULL bounds map to never-trigger sentinels, so the only
    // possible exit is expiry at the last pre-expiry tick's real price.
    void null_bounds_position_still_expires() {
        auto strat = register_strategy(QStringLiteral("test_null_k"), QStringLiteral("XKK-USD"),
                                       QJsonObject{{"notional_usd", 100.0}, {"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 11000000;
        const qint64 expires_at = t0 + 50000;
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,NULL,NULL,?,?,?,?,?,?,?)",
            {QStringLiteral("pos-k1"), strat.value(), QStringLiteral("dec-k1"), QStringLiteral("XKK-USD"),
             QStringLiteral("long"), 1.0, 100.0, expires_at, QStringLiteral("open"), t0, 0.4,
             QStringLiteral("ok"), 100.0, t0});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        write_lines(daemon.filePath("scalp_ticks.jsonl"),
                   {tick_line(QStringLiteral("XKK-USD"), 101.0, t0 + 1000)});

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), expires_at);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().closed, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-k1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("expiry"));
        QVERIFY(row.has_realized_pnl);
        // Real close at the pre-expiry tick's price 101:
        // (101 - 100) * 1.0 - 0.4 (entry) - 0.5 (exit taker on notional) = 0.1
        QVERIFY(qAbs(row.realized_pnl - 0.1) < 1e-9);
        QVERIFY(qAbs(row.exit_fee - 0.5) < 1e-9);
        QVERIFY(has_fill(row.position_id, QStringLiteral("expiry")));
    }

    // Regression (review fix 3): the prediction/hypothetical lanes must not
    // backfill positions from gate-pass journal rows older than params
    // max_age_sec. A stale row is skipped entirely; a fresh row for the
    // same book opens (also exercising the hypothetical lane end-to-end:
    // side from the row, entry at features_json.reference_price, target/
    // stop from params bps, hypothetical=1, state 'open').
    void stale_journal_rows_are_not_backfilled() {
        auto strat = register_strategy(
            QStringLiteral("test_hypo_l"), QStringLiteral("XLL-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-l"}, {"hypothetical", true},
                        {"max_age_sec", 600}, {"target_bps", 100.0}, {"stop_bps", 45.0},
                        {"horizon_sec", kFarHorizonSec}, {"taker_bps", 60.0}});
        QVERIFY(strat.is_ok());

        // Stays in the same low-time regime as every other test's synthetic
        // clock (t0 ~ 1e7): rows here must age out of LATER tests' cutoff
        // windows too, or they'd leak into those cycles' opened counts.
        const qint64 now_ms = 12000000; // now - 601s = 11399000 > 0
        // Stale: 601s old (one second past max_age_sec).
        insert_journal_row(QStringLiteral("dec-l-stale"), QStringLiteral("journal-l"), QStringLiteral("XLL-USD"),
                           QStringLiteral("long"), QStringLiteral(""), QStringLiteral("pass"), 0.9,
                           now_ms - 601 * 1000, QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});
        // Fresh: 100s old.
        insert_journal_row(QStringLiteral("dec-l-fresh"), QStringLiteral("journal-l"), QStringLiteral("XLL-USD"),
                           QStringLiteral("long"), QStringLiteral(""), QStringLiteral("pass"), 0.9,
                           now_ms - 100 * 1000, QStringLiteral("5m"), QJsonObject{{"reference_price", 200.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), now_ms);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);
        QCOMPARE(count_positions(QStringLiteral("dec-l-stale")), 0);

        const PositionRow row = fetch_position(QStringLiteral("dec-l-fresh"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
        QCOMPARE(row.side, QStringLiteral("long"));
        QVERIFY(row.hypothetical);
        QVERIFY(qAbs(row.limit_price - 200.0) < 1e-9);
        QVERIFY(row.has_target);
        QVERIFY(qAbs(row.target_price - 200.0 * 1.01) < 1e-6);  // +100 bps
        QVERIFY(row.has_stop);
        QVERIFY(qAbs(row.stop_price - 200.0 * 0.9955) < 1e-6);  // -45 bps
        QVERIFY(qAbs(row.entry_fee - (100.0 * 60.0 / 10000.0)) < 1e-9);
        QVERIFY(qAbs(fill_fee(row.position_id, QStringLiteral("open")) - row.entry_fee) < 1e-9);
    }

    // Regression (review fix 4 / minor 5): a freshness_json providing ONLY
    // live_sources (< 2, no freshest_age_ms) must classify 'degraded' --
    // the old || logic defaulted the missing age to 0 and marked it 'ok'.
    // ('unknown' with NO fields is pinned by the prediction test; 'ok' with
    // both fields healthy by test (a).)
    void live_sources_only_freshness_classifies_degraded() {
        auto strat = register_strategy(
            QStringLiteral("test_spot_m"), QStringLiteral("XMM-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-m"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", kFarHorizonSec}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 13000000;
        insert_journal_row(QStringLiteral("dec-m1"), QStringLiteral("journal-m"), QStringLiteral("XMM-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"live_sources", 1}});  // no freshest_age_ms at all

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-m1"));
        QVERIFY(row.found);
        QCOMPARE(row.data_quality, QStringLiteral("degraded"));
    }

    // Regression: the executor's PREDICTION lane opens a 'yes' position from a
    // gate-pass journal row matched by source, and migration v058's per-strategy
    // dedup lets several prediction books that share ONE source each open their
    // own position from a single matching row (none starving the others).
    //
    // Uses manually-registered prediction books rather than the seed set: kalshi
    // was retired 2026-07-07 (its journal-kalshi-scan producer is a classifier
    // with no pricing model -- gate rejects 100%, so the books could never be
    // fed) and is no longer seeded (SandboxRegistry retire_removed_kinds). The
    // executor still routes any active prediction:true book through this lane,
    // which is what this test pins.
    void prediction_lane_opens_and_dedups_per_strategy() {
        QStringList reg_ids;
        for (int hz : {900, 3600, 86400}) {
            auto r = register_strategy(
                QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
                QJsonObject{{"notional_usd", 50.0},
                            {"source", "edge_journal"},
                            {"journal_source", "edge journal-kalshi-scan"},
                            {"max_age_sec", hz},
                            {"prediction", true},
                            {"horizon_sec", hz}},
                QStringLiteral("test prediction book"));
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
            reg_ids << r.value();
        }
        QCOMPARE(reg_ids.size(), 3);

        const qint64 t0 = 14000000;
        // gate='pass', a market_probability, created_at within every book's
        // max_age_sec window -- same shape as a real journal-kalshi-scan row.
        insert_journal_row(QStringLiteral("dec-kalshi-seed-1"), QStringLiteral("edge journal-kalshi-scan"),
                           QStringLiteral("BTC-USD"), QStringLiteral(""), QStringLiteral(""),
                           QStringLiteral("pass"), 0.0, t0, QStringLiteral(""), QJsonObject{}, QJsonObject{},
                           /*market_probability=*/0.42);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 3);

        auto pr = Database::instance().execute(
            "SELECT strategy_id, side, hypothetical, limit_price, state FROM sandbox_position"
            " WHERE decision_id = ? ORDER BY strategy_id",
            {QStringLiteral("dec-kalshi-seed-1")});
        QVERIFY(pr.is_ok());
        auto& pq = pr.value();
        QSet<QString> opened_strategy_ids;
        while (pq.next()) {
            opened_strategy_ids.insert(pq.value(0).toString());
            QCOMPARE(pq.value(1).toString(), QStringLiteral("yes"));
            QCOMPARE(pq.value(2).toInt() != 0, false);
            QVERIFY(qAbs(pq.value(3).toDouble() - 0.42) < 1e-9);
            QCOMPARE(pq.value(4).toString(), QStringLiteral("open"));
        }
        QCOMPARE(opened_strategy_ids, QSet<QString>(reg_ids.begin(), reg_ids.end()));
    }

    void prediction_lane_caps_exposure_and_dedups_market_snapshots() {
        auto strategy = register_strategy(
            QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
            QJsonObject{{"notional_usd", 25.0},
                        {"source", "edge_journal"},
                        {"journal_source", "kalshi-cap-test"},
                        {"max_age_sec", 3600},
                        {"prediction", true},
                        {"horizon", "daily"},
                        {"horizon_sec", 86400}},
            QStringLiteral("prediction exposure guard"));
        QVERIFY(strategy.is_ok());

        const qint64 t0 = 14500000;
        for (int i = 0; i < 6; ++i) {
            const QString market_id = i < 3 ? QStringLiteral("DUPLICATE-MARKET")
                                             : QStringLiteral("DISTINCT-%1").arg(i);
            insert_journal_row(QStringLiteral("dec-cap-%1").arg(i), QStringLiteral("kalshi-cap-test"),
                               QStringLiteral("BTC-USD"), QStringLiteral("yes"),
                               QStringLiteral("candidate"), QStringLiteral("pass"), 0.8,
                               t0 + i, QStringLiteral("daily"), QJsonObject{}, QJsonObject{},
                               0.40 + i * 0.01, market_id);
        }

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 3);

        auto opened = Database::instance().execute(
            "SELECT COUNT(*), COUNT(DISTINCT journal.market_id)"
            " FROM sandbox_position position"
            " JOIN edge_decision_journal journal ON journal.id=position.decision_id"
            " WHERE position.strategy_id=? AND position.state='open'",
            {strategy.value()});
        QVERIFY(opened.is_ok());
        QVERIFY(opened.value().next());
        QCOMPARE(opened.value().value(0).toInt(), 3);
        QCOMPARE(opened.value().value(1).toInt(), 3);
    }

    void kalshi_parallel_paper_is_uncapped_but_uses_live_micro_evidence_gate() {
        auto strategy = register_strategy(
            QStringLiteral("kalshi_parallel_paper_test"), QStringLiteral("BTC-USD"),
            QJsonObject{{"notional_usd", 2.0}, {"journal_source", "kalshi auto-plan"},
                        {"max_age_sec", 5}, {"max_open_positions", 0}, {"prediction", true},
                        {"horizon", "parallel-test"}, {"horizon_sec", 600},
                        {"allowed_side", "both"}},
            QStringLiteral("uncapped paper with live-equivalent evidence gate"));
        QVERIFY(strategy.is_ok());

        const qint64 t0 = 14600000;
        const auto signal = [](double depth, double confidence, qint64 quote_ts) {
            return QJsonObject{{"signal", QJsonObject{{"selected_depth", depth},
                                                       {"model_confidence", confidence},
                                                       {"quote_observed_at_ms", QString::number(quote_ts)},
                                                       {"yes_bid", 0.49}, {"no_bid", 0.49}}}};
        };
        for (int i = 0; i < 8; ++i) {
            insert_journal_row(QStringLiteral("parallel-valid-%1").arg(i), QStringLiteral("kalshi auto-plan"),
                               QStringLiteral("BTC-USD"), i % 2 == 0 ? QStringLiteral("yes") : QStringLiteral("no"),
                               QStringLiteral("candidate"), QStringLiteral("pass"), 0.9, t0 + i,
                               QStringLiteral("parallel-test"), signal(20.0, 0.90, t0), QJsonObject{},
                               0.40, QStringLiteral("PARALLEL-VALID-%1").arg(i), 600, 0.01, 0.10);
        }
        insert_journal_row(QStringLiteral("parallel-stale"), QStringLiteral("kalshi auto-plan"),
                           QStringLiteral("BTC-USD"), QStringLiteral("yes"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.9, t0 + 20, QStringLiteral("parallel-test"),
                           signal(20.0, 0.90, t0 - 10'000), QJsonObject{}, 0.40,
                           QStringLiteral("PARALLEL-STALE"), 600, 0.01, 0.10);
        insert_journal_row(QStringLiteral("parallel-shallow"), QStringLiteral("kalshi auto-plan"),
                           QStringLiteral("BTC-USD"), QStringLiteral("yes"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.9, t0 + 21, QStringLiteral("parallel-test"),
                           signal(0.5, 0.90, t0), QJsonObject{}, 0.40,
                           QStringLiteral("PARALLEL-SHALLOW"), 600, 0.01, 0.10);
        insert_journal_row(QStringLiteral("parallel-low-confidence"), QStringLiteral("kalshi auto-plan"),
                           QStringLiteral("BTC-USD"), QStringLiteral("yes"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.9, t0 + 22, QStringLiteral("parallel-test"),
                           signal(20.0, 0.49, t0), QJsonObject{}, 0.40,
                           QStringLiteral("PARALLEL-LOW-CONFIDENCE"), 600, 0.01, 0.10);
        insert_journal_row(QStringLiteral("parallel-low-edge"), QStringLiteral("kalshi auto-plan"),
                           QStringLiteral("BTC-USD"), QStringLiteral("yes"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.9, t0 + 23, QStringLiteral("parallel-test"),
                           signal(20.0, 0.90, t0), QJsonObject{}, 0.40,
                           QStringLiteral("PARALLEL-LOW-EDGE"), 600, 0.01, 0.049);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");

        auto opened = Database::instance().execute(
            "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id=? AND state='open'", {strategy.value()});
        QVERIFY(opened.is_ok());
        QVERIFY(opened.value().next());
        QCOMPARE(opened.value().value(0).toInt(), 8);
        QVERIFY2(count_positions(QStringLiteral("parallel-stale")) == 0, "stale quote must be rejected");
        QVERIFY2(count_positions(QStringLiteral("parallel-shallow")) == 0, "shallow quote must be rejected");
        QVERIFY2(count_positions(QStringLiteral("parallel-low-confidence")) == 0,
                 "low model confidence must be rejected");
        QVERIFY2(count_positions(QStringLiteral("parallel-low-edge")) == 0,
                 "time-conditioned net edge must be rejected");
    }

    void prediction_lane_early_exits_at_executable_held_side_bid() {
        auto strategy = register_strategy(
            QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
            QJsonObject{{"notional_usd", 25.0}, {"source", "edge_journal"},
                        {"journal_source", "kalshi-exit-test"}, {"max_age_sec", 10},
                        {"prediction", true}, {"horizon", "1h"}, {"horizon_sec", 3600},
                        {"exit_policy", "managed"},
                        {"stop_loss_pct", 0.20}, {"take_profit_pct", 0.20}, {"taker_bps", 100.0}},
            QStringLiteral("prediction early exit"));
        QVERIFY(strategy.is_ok());

        const qint64 t0 = 14700000;
        insert_journal_row(QStringLiteral("dec-exit-open"), QStringLiteral("kalshi-exit-test"),
                           QStringLiteral("BTC-USD"), QStringLiteral("yes"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.8, t0, QStringLiteral("1h"),
                           QJsonObject{{"signal", QJsonObject{{"yes_bid", 0.49}, {"no_bid", 0.49}}}},
                           QJsonObject{}, 0.50, QStringLiteral("EXIT-MARKET"));

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto opened = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY(opened.is_ok());
        QCOMPARE(opened.value().opened, 1);

        insert_journal_row(QStringLiteral("dec-exit-stop"), QStringLiteral("kalshi-exit-test"),
                           QStringLiteral("BTC-USD"), QStringLiteral("no"), QStringLiteral("candidate"),
                           QStringLiteral("pass"), 0.8, t0 + 2000, QStringLiteral("1h"),
                           QJsonObject{{"signal", QJsonObject{{"yes_bid", 0.35}, {"no_bid", 0.63}}}},
                           QJsonObject{}, 0.36, QStringLiteral("EXIT-MARKET"));
        auto exited = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 3000);
        QVERIFY2(exited.is_ok(), exited.is_err() ? exited.error().c_str() : "");
        QCOMPARE(exited.value().closed, 1);

        auto no_reentry = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 4000);
        QVERIFY2(no_reentry.is_ok(), no_reentry.is_err() ? no_reentry.error().c_str() : "");
        QCOMPARE(no_reentry.value().opened, 0);
        auto same_market_count = Database::instance().execute(
            "SELECT COUNT(*) FROM sandbox_position position"
            " JOIN edge_decision_journal journal ON journal.id=position.decision_id"
            " WHERE position.strategy_id=? AND journal.market_id='EXIT-MARKET'",
            {strategy.value()});
        QVERIFY(same_market_count.is_ok());
        QVERIFY(same_market_count.value().next());
        QCOMPARE(same_market_count.value().value(0).toInt(), 1);

        const PositionRow position = fetch_position(QStringLiteral("dec-exit-open"));
        QVERIFY(position.found);
        QCOMPARE(position.state, QStringLiteral("closed"));
        QCOMPARE(position.close_reason, QStringLiteral("stop_loss"));
        QVERIFY(position.has_realized_pnl);
        QVERIFY(position.realized_pnl < 0.0);
    }

    void prediction_cohort_filters_time_side_and_extreme_prices() {
        auto strategy = register_strategy(
            QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
            QJsonObject{{"notional_usd", 25.0}, {"source", "edge_journal"},
                        {"journal_source", "kalshi-cohort-test"}, {"max_age_sec", 3600},
                        {"prediction", true}, {"horizon", "15m"}, {"horizon_sec", 900},
                        {"min_seconds_left", 61}, {"max_seconds_left", 300},
                        {"allowed_side", "no"}, {"min_entry_probability", 0.05},
                        {"max_entry_probability", 0.95}, {"exit_policy", "settlement"}},
            QStringLiteral("cohort boundary test"));
        QVERIFY(strategy.is_ok());

        const qint64 t0 = 15'500'000;
        insert_journal_row("cohort-too-late", "kalshi-cohort-test", "BTC-USD", "no",
                           "candidate", "pass", 0.8, t0, "15m", {}, {}, 0.40, "M-LATE", 30);
        insert_journal_row("cohort-wrong-side", "kalshi-cohort-test", "BTC-USD", "yes",
                           "candidate", "pass", 0.8, t0 + 1, "15m", {}, {}, 0.40, "M-YES", 120);
        insert_journal_row("cohort-longshot", "kalshi-cohort-test", "BTC-USD", "no",
                           "candidate", "pass", 0.8, t0 + 2, "15m", {}, {}, 0.01, "M-TINY", 180);
        insert_journal_row("cohort-valid", "kalshi-cohort-test", "BTC-USD", "no",
                           "candidate", "pass", 0.8, t0 + 3, "15m", {}, {}, 0.40, "M-VALID", 180, 0.01);
        insert_journal_row("cohort-too-early", "kalshi-cohort-test", "BTC-USD", "no",
                           "candidate", "pass", 0.8, t0 + 4, "15m", {}, {}, 0.40, "M-EARLY", 400);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");

        auto selected = Database::instance().execute(
            "SELECT decision_id, expires_at, entry_fee FROM sandbox_position WHERE strategy_id=?",
            {strategy.value()});
        QVERIFY(selected.is_ok());
        QVERIFY(selected.value().next());
        QCOMPARE(selected.value().value(0).toString(), QStringLiteral("cohort-valid"));
        QCOMPARE(selected.value().value(1).toLongLong(), t0 + 3 + 180 * 1000);
        QVERIFY(qAbs(selected.value().value(2).toDouble() - 0.625) < 1e-9);
        QVERIFY(!selected.value().next());
    }

    void settlement_policy_ignores_early_exit_signals() {
        auto strategy = register_strategy(
            QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
            QJsonObject{{"notional_usd", 25.0}, {"source", "edge_journal"},
                        {"journal_source", "kalshi-settlement-test"}, {"max_age_sec", 30},
                        {"prediction", true}, {"horizon", "15m"}, {"horizon_sec", 900},
                        {"exit_policy", "settlement"}, {"stop_loss_pct", 0.05}},
            QStringLiteral("hold to settlement"));
        QVERIFY(strategy.is_ok());
        const qint64 t0 = 15'700'000;
        insert_journal_row("settlement-open", "kalshi-settlement-test", "BTC-USD", "yes",
                           "candidate", "pass", 0.8, t0, "15m",
                           QJsonObject{{"signal", QJsonObject{{"yes_bid", 0.49}, {"no_bid", 0.49}}}},
                           {}, 0.50, "SETTLE-MARKET", 300);
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto opened = run_cycle("default", daemon.path(), t0 + 1000);
        QVERIFY(opened.is_ok());
        insert_journal_row("settlement-adverse", "kalshi-settlement-test", "BTC-USD", "no",
                           "snapshot", "watch", 0.8, t0 + 2000, "15m",
                           QJsonObject{{"signal", QJsonObject{{"yes_bid", 0.10}, {"no_bid", 0.89}}}},
                           {}, 0.90, "SETTLE-MARKET", 298);
        auto repriced = run_cycle("default", daemon.path(), t0 + 3000);
        QVERIFY(repriced.is_ok());
        const PositionRow position = fetch_position("settlement-open");
        QCOMPARE(position.state, QStringLiteral("open"));
        QVERIFY(!position.has_realized_pnl);
    }

    // Chronos-2 journal rows are price forecasts, not binary yes/no
    // contracts. The seeded chronos2 book must therefore open a concrete
    // tick-resolved paper position, preserving down forecasts as sell/short
    // economics with target below entry and stop above entry.
    void chronos_seed_book_opens_price_forecast_as_concrete_position() {
        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");

        const qint64 t0 = 15000000;
        insert_journal_row(QStringLiteral("dec-chronos-seed-1"), QStringLiteral("chronos2-forecast"),
                           QStringLiteral("BTC-USD"), QStringLiteral("sell"),
                           QStringLiteral("SELL CANDIDATE"), QStringLiteral("pass"), 0.88, t0,
                           QStringLiteral("15m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-chronos-seed-1"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
        QCOMPARE(row.side, QStringLiteral("sell"));
        QVERIFY(!row.hypothetical);
        QVERIFY(qAbs(row.limit_price - 100.0) < 1e-9);
        QVERIFY(row.has_target);
        QVERIFY(qAbs(row.target_price - 99.55) < 1e-9);   // -45 bps for sell/short
        QVERIFY(row.has_stop);
        QVERIFY(qAbs(row.stop_price - 100.25) < 1e-9);    // +25 bps for sell/short
        QVERIFY(qAbs(row.entry_fee - (50.0 * 60.0 / 10000.0)) < 1e-9);

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        QString opened_kind;
        for (const auto& r : rows.value())
            if (r.strategy_id == row.strategy_id) opened_kind = r.kind;
        QCOMPARE(opened_kind, QStringLiteral("chronos2"));
    }

    // Task 1 (v058): two active spot books with DISTINCT strategy_ids that
    // share the same journal source (e.g. spot_1h + spot_4h both reading
    // 'edge crypto-recommend') must NOT starve each other -- each opens its
    // own position from the same gate-pass journal row (per-strategy dedup,
    // migration v058's UNIQUE(strategy_id, decision_id) + the executor's
    // anti-join scoped by strategy_id). A second cycle must not double-open
    // either book's position.
    void two_books_same_source_both_open_without_starving() {
        auto strat_n = register_strategy(
            QStringLiteral("test_two_n"), QStringLiteral("XNN-USD"),
            QJsonObject{{"notional_usd", 100.0}, {"journal_source", "journal-n"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 5.0},
                        {"stop_move_pct", 2.0}, {"horizon_sec", kFarHorizonSec}});
        QVERIFY(strat_n.is_ok());
        auto strat_o = register_strategy(
            QStringLiteral("test_two_o"), QStringLiteral("XNN-USD"),
            QJsonObject{{"notional_usd", 200.0}, {"journal_source", "journal-n"}, {"min_confidence", 0.0},
                        {"min_horizon_sec", 0}, {"max_age_sec", 3600}, {"target_move_pct", 3.0},
                        {"stop_move_pct", 1.0}, {"horizon_sec", kFarHorizonSec}});
        QVERIFY(strat_o.is_ok());
        QVERIFY2(strat_n.value() != strat_o.value(), "the two books must be distinct strategy_ids");

        const qint64 t0 = 17000000;
        insert_journal_row(QStringLiteral("dec-n1"), QStringLiteral("journal-n"), QStringLiteral("XNN-USD"),
                           QStringLiteral("buy"), QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.9, t0,
                           QStringLiteral("5m"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 2);

        auto r = Database::instance().execute(
            "SELECT strategy_id, notional_usd FROM sandbox_position WHERE decision_id = ? ORDER BY strategy_id",
            {QStringLiteral("dec-n1")});
        QVERIFY(r.is_ok());
        auto& q = r.value();
        int count = 0;
        while (q.next()) {
            ++count;
            const QString sid = q.value(0).toString();
            QVERIFY(sid == strat_n.value() || sid == strat_o.value());
        }
        QCOMPARE(count, 2);

        // Second cycle: no double-open for either book.
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 2000);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().opened, 0);
        QCOMPARE(count_positions(QStringLiteral("dec-n1")), 2);
    }

    void chronos_horizon_books_do_not_cross_feed() {
        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");

        const qint64 t0 = 16000000;
        insert_journal_row(QStringLiteral("dec-chronos-1h-only"), QStringLiteral("chronos2-forecast"),
                           QStringLiteral("BTC-USD"), QStringLiteral("buy"),
                           QStringLiteral("BUY CANDIDATE"), QStringLiteral("pass"), 0.82, t0,
                           QStringLiteral("1h"), QJsonObject{{"reference_price", 100.0}},
                           QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}});

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 1);

        const PositionRow row = fetch_position(QStringLiteral("dec-chronos-1h-only"));
        QVERIFY(row.found);
        QCOMPARE(row.side, QStringLiteral("buy"));
        QVERIFY(qAbs(row.target_price - 101.0) < 1e-9);  // +100 bps for 1h book
        QVERIFY(qAbs(row.stop_price - 99.5) < 1e-9);     // -50 bps for 1h book

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        QString opened_kind;
        for (const auto& r : rows.value())
            if (r.strategy_id == row.strategy_id) opened_kind = r.kind;
        QCOMPARE(opened_kind, QStringLiteral("chronos2_1h"));
    }

    void maker_quote_refresh_replaces_pending_instead_of_stacking() {
        const QJsonObject params{
            {"notional_usd", 100.0}, {"liquidity", "maker"}, {"venue", "coinbase_advanced"},
            {"maker_bps", 40.0}, {"taker_bps", 60.0}, {"half_spread_bps", 2.0},
            {"maker_fill_through_bps", 5.0}, {"target_bps", 90.0}, {"stop_bps", 45.0},
            {"horizon_sec", kFarHorizonSec}, {"max_age_sec", 15},
            {"source", "maker_decisions"}, {"paper_only", true}};
        auto strat = register_strategy(QStringLiteral("maker"), QStringLiteral("XMR-USD"), params);
        QVERIFY2(strat.is_ok(), strat.is_err() ? strat.error().c_str() : "");

        const qint64 t0 = 5800000;
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto quote_line = [&](const QString& side, double price, qint64 ts) {
            return QStringLiteral(
                       R"({"symbol":"XMR-USD","venue":"coinbase_advanced","side":"%1",)"
                       R"("action":"PAPER_MAKER_QUOTE","reference_price":%2,"ts_ms":"%3",)"
                       R"("freshest_age_ms":100,"live_sources":2})")
                .arg(side).arg(price).arg(ts);
        };
        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line("buy", 99.98, t0), quote_line("sell", 100.02, t0)});
        auto first = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 500);
        QVERIFY2(first.is_ok(), first.is_err() ? first.error().c_str() : "");

        // Sub-basis-point drift inside the default 60-second working-quote TTL
        // keeps the existing orders instead of manufacturing cancel/replace
        // rows on every sandbox cycle.
        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line("buy", 99.985, t0 + 1000), quote_line("sell", 100.025, t0 + 1000)});
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1500);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().opened, 0);
        QCOMPARE(second.value().unfilled, 0);

        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line("buy", 100.08, t0 + 2000), quote_line("sell", 100.12, t0 + 2000)});
        auto third = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 2500);
        QVERIFY2(third.is_ok(), third.is_err() ? third.error().c_str() : "");

        auto active = Database::instance().execute(
            "SELECT count(*) FROM sandbox_position WHERE strategy_id=? AND state IN ('pending_fill','open')",
            {strat.value()});
        QVERIFY(active.is_ok() && active.value().next());
        QCOMPARE(active.value().value(0).toInt(), 2);
        auto replaced = Database::instance().execute(
            "SELECT count(*) FROM sandbox_position WHERE strategy_id=? AND close_reason='replaced'",
            {strat.value()});
        QVERIFY(replaced.is_ok() && replaced.value().next());
        QCOMPARE(replaced.value().value(0).toInt(), 2);
        QCOMPARE(fetch_position(QStringLiteral("XMR-USD|buy|") + QString::number(t0 + 2000)).state,
                 QStringLiteral("pending_fill"));
        QCOMPARE(fetch_position(QStringLiteral("XMR-USD|sell|") + QString::number(t0 + 2000)).state,
                 QStringLiteral("pending_fill"));
    }

    void maker_quote_rejects_future_decisions() {
        const QJsonObject params{
            {"notional_usd", 100.0}, {"liquidity", "maker"}, {"venue", "coinbase_advanced"},
            {"maker_bps", 40.0}, {"taker_bps", 60.0}, {"half_spread_bps", 2.0},
            {"horizon_sec", kFarHorizonSec}, {"max_age_sec", 15},
            {"source", "maker_decisions"}, {"paper_only", true}};
        auto strat = register_strategy(QStringLiteral("maker"), QStringLiteral("XMF-USD"), params);
        QVERIFY2(strat.is_ok(), strat.is_err() ? strat.error().c_str() : "");

        const qint64 now_ms = 5850000;
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        write_lines(daemon.filePath("maker_decisions.jsonl"), {
            QStringLiteral(
                R"({"symbol":"XMF-USD","venue":"coinbase_advanced","side":"buy",)"
                R"("action":"PAPER_MAKER_QUOTE","reference_price":100.0,"ts_ms":"%1",)"
                R"("freshest_age_ms":100,"live_sources":2})").arg(now_ms + 1),
        });

        auto cycle = run_cycle(QStringLiteral("default"), daemon.path(), now_ms);
        QVERIFY2(cycle.is_ok(), cycle.is_err() ? cycle.error().c_str() : "");
        QCOMPARE(cycle.value().opened, 0);
        auto positions = Database::instance().execute(
            "SELECT count(*) FROM sandbox_position WHERE strategy_id=?", {strat.value()});
        QVERIFY(positions.is_ok() && positions.value().next());
        QCOMPARE(positions.value().value(0).toInt(), 0);
    }

    void maker_quote_refresh_collapses_legacy_pending_duplicates() {
        const QJsonObject params{
            {"notional_usd", 100.0}, {"liquidity", "maker"}, {"venue", "coinbase_advanced"},
            {"maker_bps", 40.0}, {"taker_bps", 60.0}, {"half_spread_bps", 2.0},
            {"maker_fill_through_bps", 5.0}, {"target_bps", 90.0}, {"stop_bps", 45.0},
            {"horizon_sec", kFarHorizonSec}, {"max_age_sec", 15},
            {"source", "maker_decisions"}, {"paper_only", true}};
        auto strat = register_strategy(QStringLiteral("maker"), QStringLiteral("XMR-USD"), params);
        QVERIFY2(strat.is_ok(), strat.is_err() ? strat.error().c_str() : "");

        const qint64 t0 = 5900000;
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto quote_line = [&](const QString& side, double price, qint64 ts) {
            return QStringLiteral(
                       R"({"symbol":"XMR-USD","venue":"coinbase_advanced","side":"%1",)"
                       R"("action":"PAPER_MAKER_QUOTE","reference_price":%2,"ts_ms":"%3",)"
                       R"("freshest_age_ms":100,"live_sources":2})")
                .arg(side).arg(price).arg(ts);
        };
        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line("buy", 99.98, t0), quote_line("sell", 100.02, t0)});
        auto first = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 500);
        QVERIFY2(first.is_ok(), first.is_err() ? first.error().c_str() : "");

        auto duplicate = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
            " notional_usd, created_at) VALUES (?,?,?,?,?,0,?,?,?,?,?,'pending_fill',NULL,0,'ok',?,?)",
            {QStringLiteral("legacy-maker-duplicate"), strat.value(), QStringLiteral("legacy-maker-decision"),
             QStringLiteral("XMR-USD"), QStringLiteral("buy"), 1.0, 99.981, 100.88, 99.53,
             t0 + kFarHorizonSec * 1000, 100.0, t0 + 100});
        QVERIFY2(duplicate.is_ok(), duplicate.is_err() ? duplicate.error().c_str() : "");

        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line("buy", 99.985, t0 + 1000), quote_line("sell", 100.025, t0 + 1000)});
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1500);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().opened, 0);
        QCOMPARE(second.value().unfilled, 1);

        auto active = Database::instance().execute(
            "SELECT side, count(*) FROM sandbox_position WHERE strategy_id=?"
            " AND state IN ('pending_fill','open') GROUP BY side ORDER BY side",
            {strat.value()});
        QVERIFY(active.is_ok());
        QVERIFY(active.value().next());
        QCOMPARE(active.value().value(0).toString(), QStringLiteral("buy"));
        QCOMPARE(active.value().value(1).toInt(), 1);
        QVERIFY(active.value().next());
        QCOMPARE(active.value().value(0).toString(), QStringLiteral("sell"));
        QCOMPARE(active.value().value(1).toInt(), 1);
        QVERIFY(!active.value().next());
    }

    // Maker spread producer end-to-end. Two-sided OPEN is proved on a shared
    // symbol (XMK); the signed round trips are proved on dedicated per-leg
    // symbols (XMB long, XMS short) so two opposing brackets never fight over one
    // tick stream. (On a single symbol a bid's long target sits above an ask's
    // short stop -- physically real, but only one leg can win per symbol; separate
    // symbols make each leg's target reachable, which is what exercises the executor.)
    void maker_quotes_open_both_legs_fill_and_book_signed_pnl() {
        const QJsonObject params{
            {"notional_usd", 100.0}, {"liquidity", "maker"}, {"venue", "coinbase_advanced"},
            {"maker_bps", 40.0}, {"taker_bps", 60.0}, {"half_spread_bps", 2.0},
            {"slippage_bps", 1.0}, {"maker_fill_through_bps", 5.0},
            {"target_bps", 90.0}, {"stop_bps", 45.0}, {"horizon_sec", kFarHorizonSec},
            {"max_age_sec", 15}, {"source", "maker_decisions"}, {"paper_only", true}};
        auto strat = register_strategy(QStringLiteral("maker"),
                                       QStringLiteral("XMK-USD,XMB-USD,XMS-USD"), params);
        QVERIFY2(strat.is_ok(), strat.is_err() ? strat.error().c_str() : "");

        const qint64 t0 = 6000000;
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto quote_line = [&](const QString& symbol, const QString& side, double price) {
            return QStringLiteral(
                       R"({"symbol":"%1","venue":"coinbase_advanced","side":"%2",)"
                       R"("liquidity":"maker","action":"PAPER_MAKER_QUOTE","reference_price":%3,)"
                       R"("ts_ms":"%4","freshest_age_ms":120,"live_sources":3})")
                .arg(symbol, side).arg(price, 0, 'f', 4).arg(t0);
        };
        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote_line(QStringLiteral("XMK-USD"), QStringLiteral("buy"), 99.98),
                     quote_line(QStringLiteral("XMK-USD"), QStringLiteral("sell"), 100.02),
                     quote_line(QStringLiteral("XMB-USD"), QStringLiteral("buy"), 99.98),
                     quote_line(QStringLiteral("XMS-USD"), QStringLiteral("sell"), 100.02)});

        // Cycle 1: all four decisions open as pending_fill. run_cycle's report
        // .opened is a GLOBAL count across every active strategy, and this
        // binary shares one DB across slots (see the file header) -- an earlier
        // slot's seeded producer-backed book can open its own leftover
        // edge_decision_journal row in the same cycle. Assert on THIS strategy's
        // four legs directly so the count is isolated from that shared state.
        auto c1 = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(c1.is_ok(), c1.is_err() ? c1.error().c_str() : "");
        auto mine = Database::instance().execute(
            "SELECT count(*) FROM sandbox_position WHERE strategy_id=?", {strat.value()});
        QVERIFY(mine.is_ok() && mine.value().next());
        QCOMPARE(mine.value().value(0).toInt(), 4);

        // Two-sided open on ONE symbol: a buy and a sell leg, mirrored brackets.
        const PositionRow kbid = fetch_position(QStringLiteral("XMK-USD|buy|") + QString::number(t0));
        const PositionRow kask = fetch_position(QStringLiteral("XMK-USD|sell|") + QString::number(t0));
        QVERIFY(kbid.found);
        QVERIFY(kask.found);
        QCOMPARE(kbid.side, QStringLiteral("buy"));
        QCOMPARE(kask.side, QStringLiteral("sell"));
        QCOMPARE(kbid.state, QStringLiteral("pending_fill"));
        QCOMPARE(kask.state, QStringLiteral("pending_fill"));
        QVERIFY(qAbs(kbid.limit_price - 99.98) < 1e-9);
        QVERIFY(qAbs(kask.limit_price - 100.02) < 1e-9);
        QVERIFY(kbid.target_price > kbid.limit_price);   // long reverts up
        QVERIFY(kbid.stop_price < kbid.limit_price);
        QVERIFY(kask.target_price < kask.limit_price);   // short reverts down
        QVERIFY(kask.stop_price > kask.limit_price);
        QCOMPARE(kbid.data_quality, QStringLiteral("ok"));

        // Cycle 2: fill the two dedicated legs by trading THROUGH each quote
        // (XMB bid on a down-tick, XMS ask on an up-tick).
        write_lines(daemon.filePath("maker_ticks.jsonl"),
                    {maker_tick_line(QStringLiteral("XMB-USD"), QStringLiteral("kraken_pro"), 99.90, t0 + 1500),
                     maker_tick_line(QStringLiteral("XMS-USD"), QStringLiteral("kraken_pro"), 100.10, t0 + 1600)});
        auto wrong_venue = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1700);
        QVERIFY2(wrong_venue.is_ok(), wrong_venue.is_err() ? wrong_venue.error().c_str() : "");
        QCOMPARE(fetch_position(QStringLiteral("XMB-USD|buy|") + QString::number(t0)).state,
                 QStringLiteral("pending_fill"));
        QCOMPARE(fetch_position(QStringLiteral("XMS-USD|sell|") + QString::number(t0)).state,
                 QStringLiteral("pending_fill"));

        write_lines(daemon.filePath("maker_ticks.jsonl"),
                    {maker_tick_line(QStringLiteral("XMB-USD"), QStringLiteral("coinbase_advanced"), 99.90, t0 + 2000),
                     maker_tick_line(QStringLiteral("XMS-USD"), QStringLiteral("coinbase_advanced"), 100.10, t0 + 2500)});
        auto c2 = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 3000);
        QVERIFY2(c2.is_ok(), c2.is_err() ? c2.error().c_str() : "");
        const PositionRow bfill = fetch_position(QStringLiteral("XMB-USD|buy|") + QString::number(t0));
        const PositionRow sfill = fetch_position(QStringLiteral("XMS-USD|sell|") + QString::number(t0));
        QCOMPARE(bfill.state, QStringLiteral("open"));
        QCOMPARE(sfill.state, QStringLiteral("open"));

        // Cycle 3: each dedicated leg reaches its OWN target on its OWN symbol,
        // so they never contend. Long target is above; short target is below.
        // A maker exit requires the tick to trade THROUGH the target by
        // maker_fill_through_bps (5 bps) -- a mere touch leaves us behind the
        // queue (see honest_maker_exit_requires_trade_through_end_to_end) -- so
        // push each tick ~10 bps past its target. The fill is still booked at
        // target_price itself, so realized pnl is independent of this overshoot.
        write_lines(daemon.filePath("maker_ticks.jsonl"),
                    {maker_tick_line(QStringLiteral("XMB-USD"), QStringLiteral("coinbase_advanced"),
                                     bfill.target_price * 1.001, t0 + 4000),
                     maker_tick_line(QStringLiteral("XMS-USD"), QStringLiteral("coinbase_advanced"),
                                     sfill.target_price * 0.999, t0 + 4500)});
        auto c3 = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 5000);
        QVERIFY2(c3.is_ok(), c3.is_err() ? c3.error().c_str() : "");

        const PositionRow bclosed = fetch_position(QStringLiteral("XMB-USD|buy|") + QString::number(t0));
        const PositionRow sclosed = fetch_position(QStringLiteral("XMS-USD|sell|") + QString::number(t0));
        QCOMPARE(bclosed.state, QStringLiteral("closed"));
        QCOMPARE(sclosed.state, QStringLiteral("closed"));
        QCOMPARE(bclosed.close_reason, QStringLiteral("target"));
        QCOMPARE(sclosed.close_reason, QStringLiteral("target"));
        QVERIFY(bclosed.has_realized_pnl);
        QVERIFY(sclosed.has_realized_pnl);
        // Both hit their target => both wins. The short leg is a win only if its
        // pnl uses (entry - exit); the buy-forced neuter turns it into a loss.
        QVERIFY2(bclosed.realized_pnl > 0.0, "long leg target hit must be a win");
        QVERIFY2(sclosed.realized_pnl > 0.0, "short leg target hit must be a win (signed pnl)");
    }

    // A maker quote is priced off its OWN venue's fresh book, so its data_quality
    // gates on that venue's tick freshness -- NOT the cross-venue live_sources
    // count (a latency/scalp heuristic). A single fresh venue (live_sources=1)
    // must be 'ok' so its resolved trades reach the cost-net scorecard; a stale
    // venue quote must still be 'degraded' so the freshness gate is not lost.
    void maker_quote_quality_gates_on_venue_freshness_not_source_count() {
        const QJsonObject params{
            {"notional_usd", 100.0}, {"liquidity", "maker"}, {"venue", "coinbase_advanced"},
            {"maker_bps", 40.0}, {"taker_bps", 60.0}, {"half_spread_bps", 2.0},
            {"slippage_bps", 1.0}, {"maker_fill_through_bps", 5.0},
            {"target_bps", 90.0}, {"stop_bps", 45.0}, {"horizon_sec", kFarHorizonSec},
            {"max_age_sec", 15}, {"source", "maker_decisions"}, {"paper_only", true}};
        auto strat = register_strategy(QStringLiteral("maker"), QStringLiteral("XMQ-USD"), params);
        QVERIFY2(strat.is_ok(), strat.is_err() ? strat.error().c_str() : "");

        const qint64 t0 = 6100000;
        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        auto quote = [&](const QString& side, double price, int age_ms) {
            return QStringLiteral(
                       R"({"symbol":"XMQ-USD","venue":"coinbase_advanced","side":"%1",)"
                       R"("liquidity":"maker","action":"PAPER_MAKER_QUOTE","reference_price":%2,)"
                       R"("ts_ms":"%3","freshest_age_ms":%4,"live_sources":1})")
                .arg(side).arg(price, 0, 'f', 4).arg(t0).arg(age_ms);
        };
        // Both single-venue (live_sources=1). Fresh bid -> ok; stale ask -> degraded.
        write_lines(daemon.filePath("maker_decisions.jsonl"),
                    {quote(QStringLiteral("buy"), 99.98, 120),
                     quote(QStringLiteral("sell"), 100.02, 9000)});

        auto c1 = run_cycle(QStringLiteral("default"), daemon.path(), t0 + 1000);
        QVERIFY2(c1.is_ok(), c1.is_err() ? c1.error().c_str() : "");
        const PositionRow fresh = fetch_position(QStringLiteral("XMQ-USD|buy|") + QString::number(t0));
        const PositionRow stale = fetch_position(QStringLiteral("XMQ-USD|sell|") + QString::number(t0));
        QVERIFY(fresh.found);
        QVERIFY(stale.found);
        QCOMPARE(fresh.data_quality, QStringLiteral("ok"));       // 1 fresh venue is enough
        QCOMPARE(stale.data_quality, QStringLiteral("degraded")); // stale venue still caught
    }
};

QTEST_GUILESS_MAIN(TstSandboxExecutor)
#include "tst_sandbox_executor.moc"
