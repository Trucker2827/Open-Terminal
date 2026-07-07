// tst_sandbox_resolver.cpp — SandboxResolver::resolve_pending (Task 8):
// settling prediction (journal outcome) and hypothetical (tick target/stop/
// expiry) books that PaperExecutor::run_cycle deliberately leaves 'open'
// forever.
//
// DB bring-up mirrors tst_sandbox_executor.cpp's initTestCase(). Every test
// below uses its own unique decision_id/position_id/symbol to stay isolated
// from every other slot within this binary.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxResolver.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services::sandbox;

namespace {

// Copies the exact DB-open incantation from tst_sandbox_executor.cpp.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

// prediction_unresolved_before_grace_stays_open() deliberately leaves its
// position 'open' at test-end; resolve_pending scans sandbox_position
// GLOBALLY (not scoped to one strategy/decision), so a later test's much
// larger now_ms could otherwise sweep this leftover row past ITS grace too
// (same leftover-position hazard tst_sandbox_executor.cpp's kFarHorizonSec
// documents). Anchoring that one test's expires_at far beyond any other
// test's now_ms keeps it inert for the rest of this binary's run.
constexpr qint64 kFarFutureMs = 900000000000LL;

QString journal_json(const QJsonObject& o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Inserts one edge_decision_journal fixture row with an explicit outcome
// (default -1, matching the schema's own DEFAULT and edge_parse_outcome's
// "pending" mapping -- see SandboxResolver.h's semantics note).
void insert_journal_row(const QString& id, const QString& source, const QString& symbol, qint64 created_at,
                         int outcome = -1) {
    auto r = Database::instance().execute(
        "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, horizon, side, call, gate,"
        " market_probability, confidence, freshness_json, features_json, source, outcome)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, created_at, created_at, symbol, QStringLiteral("5m"), QStringLiteral("yes"), QStringLiteral(""),
         QStringLiteral("pass"), 0.6, 0.9, journal_json(QJsonObject{}), journal_json(QJsonObject{}), source,
         outcome});
    QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
}

// Inserts one sandbox_position fixture row directly (same pattern as
// tst_sandbox_executor.cpp's tests (e)/(f): isolation over driving the full
// executor open flow). Covers both prediction (side 'yes') and hypothetical
// (side 'long'/'short', hypothetical=1) shapes via the same columns.
void insert_position(const QString& position_id, const QString& strategy_id, const QString& decision_id,
                      const QString& symbol, const QString& side, bool hypothetical, double qty,
                      double limit_price, const QVariant& target_price, const QVariant& stop_price,
                      qint64 expires_at, qint64 opened_at, double entry_fee, double notional_usd,
                      qint64 created_at) {
    auto r = Database::instance().execute(
        "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, entry_fee, data_quality,"
        " notional_usd, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,'open',?,?,'ok',?,?)",
        {position_id, strategy_id, decision_id, symbol, side, hypothetical ? 1 : 0, qty, limit_price,
         target_price, stop_price, expires_at, opened_at, entry_fee, notional_usd, created_at});
    QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
}

struct PositionRow {
    bool found = false;
    QString state, close_reason, data_quality;
    bool has_realized_pnl = false;
    double realized_pnl = 0, exit_fee = 0;
};

PositionRow fetch_position(const QString& position_id) {
    PositionRow row;
    auto r = Database::instance().execute(
        "SELECT state, close_reason, data_quality, realized_pnl, exit_fee FROM sandbox_position"
        " WHERE position_id = ?",
        {position_id});
    if (r.is_err() || !r.value().next())
        return row;
    auto& q = r.value();
    row.found = true;
    row.state = q.value(0).toString();
    row.close_reason = q.value(1).toString();
    row.data_quality = q.value(2).toString();
    row.has_realized_pnl = !q.value(3).isNull();
    row.realized_pnl = q.value(3).toDouble();
    row.exit_fee = q.value(4).toDouble();
    return row;
}

QString fill_note(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT note FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    if (r.is_err() || !r.value().next())
        return QStringLiteral("<no fill row>");
    return r.value().value(0).toString();
}

bool has_fill(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT count(*) FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    return r.is_ok() && r.value().next() && r.value().value(0).toInt() > 0;
}

double fill_price(const QString& position_id, const QString& kind) {
    auto r = Database::instance().execute(
        "SELECT price FROM sandbox_fill WHERE position_id = ? AND kind = ?", {position_id, kind});
    if (r.is_err() || !r.value().next())
        return -1.0;
    return r.value().value(0).toDouble();
}

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

class TstSandboxResolver : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    // (a) prediction win: journal outcome=1 -> payout 1.0/contract,
    // realized_pnl = (1.0 - entry) * qty - entry_fee, NO exit fee,
    // close_reason='resolved', sandbox_fill kind 'resolved' price 1.0.
    void prediction_win_pays_hand_computed_pnl() {
        auto strat = register_strategy(QStringLiteral("btc5m"), QStringLiteral("BTC"),
                                       QJsonObject{{"prediction", true}, {"journal_source", "journal-pred-a"}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 10000000;
        insert_journal_row(QStringLiteral("dec-pred-a"), QStringLiteral("journal-pred-a"), QStringLiteral("BTC"), t0,
                            1);
        insert_position(QStringLiteral("pos-pred-a"), strat.value(), QStringLiteral("dec-pred-a"),
                        QStringLiteral("BTC"), QStringLiteral("yes"), false, 10.0, 0.6, QVariant(), QVariant(),
                        t0 + 100000, t0, 1.5, 100.0, t0);

        auto rep = resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), t0 + 1000);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");
        QCOMPARE(rep.value().resolved, 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-pred-a"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("resolved"));
        QVERIFY(row.has_realized_pnl);
        // (1.0 - 0.6) * 10 - 1.5 = 2.5
        QVERIFY(qAbs(row.realized_pnl - 2.5) < 1e-9);
        QCOMPARE(row.exit_fee, 0.0);
        QVERIFY(has_fill(QStringLiteral("pos-pred-a"), QStringLiteral("resolved")));
        QVERIFY(qAbs(fill_price(QStringLiteral("pos-pred-a"), QStringLiteral("resolved")) - 1.0) < 1e-9);
    }

    // (b) prediction loss: journal outcome=0 -> payout 0, realized_pnl =
    // (0 - entry) * qty - entry_fee, no exit fee.
    void prediction_loss_books_hand_computed_pnl() {
        auto strat = register_strategy(QStringLiteral("btc5m"), QStringLiteral("BTC"),
                                       QJsonObject{{"prediction", true}, {"journal_source", "journal-pred-b"}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 11000000;
        insert_journal_row(QStringLiteral("dec-pred-b"), QStringLiteral("journal-pred-b"), QStringLiteral("BTC"), t0,
                            0);
        insert_position(QStringLiteral("pos-pred-b"), strat.value(), QStringLiteral("dec-pred-b"),
                        QStringLiteral("BTC"), QStringLiteral("yes"), false, 10.0, 0.6, QVariant(), QVariant(),
                        t0 + 100000, t0, 1.5, 100.0, t0);

        auto rep = resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), t0 + 1000);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");
        QCOMPARE(rep.value().resolved, 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-pred-b"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("resolved"));
        QVERIFY(row.has_realized_pnl);
        // (0.0 - 0.6) * 10 - 1.5 = -7.5
        QVERIFY(qAbs(row.realized_pnl - (-7.5)) < 1e-9);
        QVERIFY(qAbs(fill_price(QStringLiteral("pos-pred-b"), QStringLiteral("resolved")) - 0.0) < 1e-9);
    }

    // (c) prediction unresolved (outcome=-1), still within the 24h grace
    // window past expires_at -> stays open, counted pending.
    void prediction_unresolved_before_grace_stays_open() {
        auto strat = register_strategy(QStringLiteral("btc5m"), QStringLiteral("BTC"),
                                       QJsonObject{{"prediction", true}, {"journal_source", "journal-pred-c"}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 12000000;
        const qint64 expires_at = kFarFutureMs;
        insert_journal_row(QStringLiteral("dec-pred-c"), QStringLiteral("journal-pred-c"), QStringLiteral("BTC"), t0,
                            -1);
        insert_position(QStringLiteral("pos-pred-c"), strat.value(), QStringLiteral("dec-pred-c"),
                        QStringLiteral("BTC"), QStringLiteral("yes"), false, 10.0, 0.6, QVariant(), QVariant(),
                        expires_at, t0, 1.5, 100.0, t0);

        // Past raw expiry, but well within the grace window.
        const qint64 now_ms = expires_at + 1000;
        auto rep = resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), now_ms);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");
        QCOMPARE(rep.value().resolved, 0);
        QVERIFY(rep.value().pending >= 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-pred-c"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("open"));
    }

    // (d) prediction unresolved (outcome=-1), past expires_at + 24h grace ->
    // closes 'expiry' with NULL realized_pnl, fill note 'never resolved'.
    void prediction_unresolved_past_grace_closes_null_pnl() {
        auto strat = register_strategy(QStringLiteral("btc5m"), QStringLiteral("BTC"),
                                       QJsonObject{{"prediction", true}, {"journal_source", "journal-pred-d"}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 13000000;
        const qint64 expires_at = t0 + 100000;
        insert_journal_row(QStringLiteral("dec-pred-d"), QStringLiteral("journal-pred-d"), QStringLiteral("BTC"), t0,
                            -1);
        insert_position(QStringLiteral("pos-pred-d"), strat.value(), QStringLiteral("dec-pred-d"),
                        QStringLiteral("BTC"), QStringLiteral("yes"), false, 10.0, 0.6, QVariant(), QVariant(),
                        expires_at, t0, 1.5, 100.0, t0);

        const qint64 now_ms = expires_at + kResolveGraceMs + 1;
        auto rep = resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), now_ms);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");
        QCOMPARE(rep.value().resolved, 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-pred-d"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("expiry"));
        QVERIFY(!row.has_realized_pnl);
        QCOMPARE(row.data_quality, QStringLiteral("degraded"));
        QCOMPARE(fill_note(QStringLiteral("pos-pred-d"), QStringLiteral("expiry")), QStringLiteral("never resolved"));
    }

    // (e) hypothetical (long_short) position that DOES have a pre-expiry
    // tick crossing its target -> resolver must tick-resolve it with a real
    // pnl, NOT wait for expiry/grace. Regression for the discrepancy this
    // task's controller notes got wrong: advance_open_positions filters
    // hypothetical=0 (PaperExecutor.cpp), so nothing else will ever close
    // this position off ticks.
    void hypothetical_target_hit_closes_with_real_pnl() {
        auto strat = register_strategy(QStringLiteral("long_short"), QStringLiteral("XHH-USD"),
                                       QJsonObject{{"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 14000000;
        const qint64 expires_at = t0 + 100000;
        insert_position(QStringLiteral("pos-hyp-e"), strat.value(), QStringLiteral("dec-hyp-e"),
                        QStringLiteral("XHH-USD"), QStringLiteral("long"), true, 1.0, 100.0, QVariant(105.0),
                        QVariant(97.0), expires_at, t0, 0.4, 100.0, t0);

        QTemporaryDir daemon;
        QVERIFY(daemon.isValid());
        const QString ticks_path = daemon.filePath("scalp_ticks.jsonl");
        write_lines(ticks_path, {tick_line(QStringLiteral("XHH-USD"), 106.0, t0 + 500)});

        auto rep = resolve_pending(QStringLiteral("default"), ticks_path, t0 + 1000);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");
        QCOMPARE(rep.value().resolved, 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-hyp-e"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("target"));
        QVERIFY(row.has_realized_pnl);
        // (106 - 100) * 1.0 - 0.4 (entry) - 0.5 (exit taker on notional) = 5.1
        QVERIFY(qAbs(row.realized_pnl - 5.1) < 1e-9);
        QVERIFY(qAbs(row.exit_fee - 0.5) < 1e-9);
        QVERIFY(has_fill(QStringLiteral("pos-hyp-e"), QStringLiteral("target")));
    }

    // (f) hypothetical (long_short) position with NO tick data at all, past
    // expires_at + grace -> closes 'expiry' with NULL pnl, note
    // 'never resolved'. hypothetical flag stays 1 throughout.
    void hypothetical_no_ticks_past_grace_closes_null_pnl() {
        auto strat = register_strategy(QStringLiteral("long_short"), QStringLiteral("XII-USD"),
                                       QJsonObject{{"taker_bps", 50.0}});
        QVERIFY(strat.is_ok());

        const qint64 t0 = 15000000;
        const qint64 expires_at = t0 + 100000;
        insert_position(QStringLiteral("pos-hyp-f"), strat.value(), QStringLiteral("dec-hyp-f"),
                        QStringLiteral("XII-USD"), QStringLiteral("long"), true, 1.0, 100.0, QVariant(105.0),
                        QVariant(97.0), expires_at, t0, 0.4, 100.0, t0);

        // Before grace: no ticks file at all -> stays open, pending.
        const qint64 before_grace = expires_at + 1000;
        auto rep1 =
            resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), before_grace);
        QVERIFY2(rep1.is_ok(), rep1.is_err() ? rep1.error().c_str() : "");
        QCOMPARE(rep1.value().resolved, 0);
        QVERIFY(rep1.value().pending >= 1);
        QCOMPARE(fetch_position(QStringLiteral("pos-hyp-f")).state, QStringLiteral("open"));

        // Past grace: still no ticks -> closes 'expiry', NULL pnl.
        const qint64 past_grace = expires_at + kResolveGraceMs + 1;
        auto rep2 =
            resolve_pending(QStringLiteral("default"), QStringLiteral("/nonexistent/ticks.jsonl"), past_grace);
        QVERIFY2(rep2.is_ok(), rep2.is_err() ? rep2.error().c_str() : "");
        QCOMPARE(rep2.value().resolved, 1);

        const PositionRow row = fetch_position(QStringLiteral("pos-hyp-f"));
        QVERIFY(row.found);
        QCOMPARE(row.state, QStringLiteral("closed"));
        QCOMPARE(row.close_reason, QStringLiteral("expiry"));
        QVERIFY(!row.has_realized_pnl);
        QCOMPARE(fill_note(QStringLiteral("pos-hyp-f"), QStringLiteral("expiry")), QStringLiteral("never resolved"));
    }
};

QTEST_GUILESS_MAIN(TstSandboxResolver)
#include "tst_sandbox_resolver.moc"
