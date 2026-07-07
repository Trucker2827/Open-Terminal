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
                        double market_probability = 0.0) {
    auto r = Database::instance().execute(
        "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, horizon, side, call, gate,"
        " market_probability, confidence, freshness_json, features_json, source)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, created_at, created_at, symbol, horizon, side, call, gate, market_probability, confidence,
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
            QJsonObject{{"notional_usd", 50.0}, {"max_age_sec", 15}, {"entry_offset_bps", 1.0},
                        {"target_bps", 25.0}, {"stop_bps", 15.0}, {"horizon_sec", 900}});
        QVERIFY(strat.is_ok());

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const qint64 ts_ms = now_ms - 2000; // within the 15s max_age_sec window
        const QString decision = QStringLiteral(
                                     R"({"symbol":"XSS-USD","verdict":"PAPER TRADE CANDIDATE",)"
                                     R"("action":"PAPER_LIMIT_BUY_ONLY","ts_ms":"%1","reference_price":200.0,)"
                                     R"("freshest_age_ms":50,"live_sources":3})")
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

        // Same fixture, second cycle -> scalp's soft pre-check dedup (not
        // the neuter-verified SQL anti-join, which is spot-only) means no
        // second position and no error.
        auto second = run_cycle(QStringLiteral("default"), daemon.path(), now_ms + 100);
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().opened, 0);
        QCOMPARE(count_positions(decision_id), 1);
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
};

QTEST_GUILESS_MAIN(TstSandboxExecutor)
#include "tst_sandbox_executor.moc"
