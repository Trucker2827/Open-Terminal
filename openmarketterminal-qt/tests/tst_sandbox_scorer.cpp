// tst_sandbox_scorer.cpp — SandboxScorer::score_all / leaderboard (Task 9):
// sandbox_score day-bucketed rollups plus the season-to-date leaderboard
// aggregation over sandbox_position.
//
// DB bring-up mirrors tst_sandbox_resolver.cpp's initTestCase(). Every test
// below registers its own strategy (content-addressed by kind/symbols/params
// -- see SandboxRegistry.h) and uses unique position ids, so tests stay
// isolated from each other within this binary's shared DB/process.

#include <QtTest>
#include <QTemporaryDir>
#include <QDateTime>
#include <QTimeZone>
#include <QJsonObject>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services::sandbox;

namespace {

bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

// UTC calendar-day anchor, well clear of any other test file's fixture
// timestamps (which live in the tens-of-millions-of-ms range) -- see
// tst_sandbox_resolver.cpp's kFarFutureMs comment for why cross-test leftover
// rows matter: score_all scans sandbox_position GLOBALLY, not scoped to one
// strategy, so an isolated day per test slot keeps "yesterday/today" bucket
// membership from one slot ever brushing against another's fixture rows.
qint64 day_anchor_ms(int day_offset) {
    return QDateTime(QDate(2030, 1, 1).addDays(day_offset), QTime(2, 0), QTimeZone::UTC).toMSecsSinceEpoch();
}

QString day_string(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).date().toString(Qt::ISODate);
}

// Inserts one sandbox_position fixture row with every column the scorer
// reads. `realized_pnl`/`closed_at`/`target_price`/`stop_price` are QVariant
// so NULL can be expressed directly (QVariant()).
void insert_position(const QString& position_id, const QString& strategy_id, const QString& state,
                      const QVariant& realized_pnl, double notional_usd, const QString& data_quality,
                      const QVariant& closed_at, const QString& close_reason = QString(),
                      qint64 created_at = 1) {
    static int counter = 0;
    const QString decision_id = QStringLiteral("dec-%1-%2").arg(position_id).arg(++counter);
    auto r = Database::instance().execute(
        "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee, exit_fee, realized_pnl,"
        " close_reason, data_quality, notional_usd, created_at)"
        " VALUES (?,?,?,?,?,0,1.0,100.0,?,?,?,?,0.1,0.1,?,?,?,?,?)",
        {position_id, strategy_id, decision_id, QStringLiteral("BTC-USD"), QStringLiteral("buy"),
         created_at + 1000000, state, created_at,
         closed_at, realized_pnl,
         close_reason.isEmpty() ? QVariant() : QVariant(close_reason), data_quality, notional_usd, created_at});
    QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
}

struct ScoreRow {
    bool found = false;
    int resolved_count = 0, open_count = 0, unfilled_count = 0, degraded_count = 0;
    double net_pnl = 0, hit_rate = 0, avg_win = 0, avg_loss = 0, max_drawdown = 0, gross_notional = 0;
};

ScoreRow fetch_score(const QString& strategy_id, const QString& score_date) {
    ScoreRow row;
    auto r = Database::instance().execute(
        "SELECT resolved_count, open_count, unfilled_count, net_pnl, hit_rate, avg_win, avg_loss,"
        " max_drawdown, degraded_count, gross_notional FROM sandbox_score"
        " WHERE strategy_id = ? AND score_date = ?",
        {strategy_id, score_date});
    if (r.is_err() || !r.value().next())
        return row;
    auto& q = r.value();
    row.found = true;
    row.resolved_count = q.value(0).toInt();
    row.open_count = q.value(1).toInt();
    row.unfilled_count = q.value(2).toInt();
    row.net_pnl = q.value(3).toDouble();
    row.hit_rate = q.value(4).toDouble();
    row.avg_win = q.value(5).toDouble();
    row.avg_loss = q.value(6).toDouble();
    row.max_drawdown = q.value(7).toDouble();
    row.degraded_count = q.value(8).toInt();
    row.gross_notional = q.value(9).toDouble();
    return row;
}

const LeaderboardRow* find_row(const QList<LeaderboardRow>& rows, const QString& strategy_id) {
    for (const auto& r : rows) {
        if (r.strategy_id == strategy_id)
            return &r;
    }
    return nullptr;
}

} // namespace

class TstSandboxScorer : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    // (a) The brief's hand-computed fixture: 3 closed (+2.40, -1.10, +0.30)
    // + 1 unfilled + 1 open + 1 NULL-pnl degraded-closed (data-gap) row, all
    // on the same UTC day. Expect resolved=3, net=1.60, hit=2/3, avg_win=
    // 1.35, avg_loss=-1.10, drawdown=1.10 -- UNCHANGED by the data-gap row's
    // presence (the discriminating test for contract 1: a COALESCE(0) bug
    // would pull net_pnl down to 1.60 + 0 == still 1.60 by coincidence here,
    // but would corrupt resolved_count to 4 and hit_rate to 2/4 -- assert
    // both). degraded_count must be 1 (only the data-gap row is
    // data_quality='degraded'), unfilled_count 1, open_count 1.
    void score_all_computes_hand_verified_aggregates_and_excludes_null_pnl() {
        auto strat = register_strategy(QStringLiteral("spot"), QStringLiteral("BTC-USD"),
                                       QJsonObject{{"case", "a"}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        const qint64 day0 = day_anchor_ms(0);
        insert_position(QStringLiteral("scorer-a-1"), sid, QStringLiteral("closed"), 2.40, 100.0,
                        QStringLiteral("ok"), day0 + 1000, QStringLiteral("target"), day0 + 1000);
        insert_position(QStringLiteral("scorer-a-2"), sid, QStringLiteral("closed"), -1.10, 100.0,
                        QStringLiteral("ok"), day0 + 2000, QStringLiteral("stop"), day0 + 2000);
        insert_position(QStringLiteral("scorer-a-3"), sid, QStringLiteral("closed"), 0.30, 100.0,
                        QStringLiteral("ok"), day0 + 3000, QStringLiteral("target"), day0 + 3000);
        insert_position(QStringLiteral("scorer-a-4"), sid, QStringLiteral("unfilled"), QVariant(), 0.0,
                        QStringLiteral("ok"), day0 + 4000, QStringLiteral("unfilled"), day0 + 4000);
        insert_position(QStringLiteral("scorer-a-5"), sid, QStringLiteral("open"), QVariant(), 100.0,
                        QStringLiteral("ok"), QVariant(), QString(), day0 + 4500);
        // Data-gap close: realized_pnl NULL, forced data_quality='degraded'
        // (PaperExecutor.h's contract for a no-pre-expiry-tick expiry).
        insert_position(QStringLiteral("scorer-a-6"), sid, QStringLiteral("closed"), QVariant(), 100.0,
                        QStringLiteral("degraded"), day0 + 5000, QStringLiteral("expiry"), day0 + 5000);

        const qint64 now_ms = day0 + 10000; // still the same UTC day.
        auto rep = score_all(QStringLiteral("default"), now_ms);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");

        const ScoreRow row = fetch_score(sid, day_string(day0));
        QVERIFY(row.found);
        QCOMPARE(row.resolved_count, 3);
        QVERIFY(qAbs(row.net_pnl - 1.60) < 1e-9);
        QVERIFY(qAbs(row.hit_rate - (2.0 / 3.0)) < 1e-9);
        QVERIFY(qAbs(row.avg_win - 1.35) < 1e-9);
        QVERIFY(qAbs(row.avg_loss - (-1.10)) < 1e-9);
        QVERIFY(qAbs(row.max_drawdown - 1.10) < 1e-9);
        QCOMPARE(row.degraded_count, 1);
        QCOMPARE(row.unfilled_count, 1);
        QCOMPARE(row.open_count, 1);
        QVERIFY(qAbs(row.gross_notional - 300.0) < 1e-9);

        // leaderboard()'s independent all-time aggregation must agree with
        // sandbox_score's stored figures for this single-day fixture, and
        // must report the data-gap row via data_gap_count (never silently
        // dropped, never folded into resolved_count).
        auto lb = leaderboard(QStringLiteral("default"));
        QVERIFY2(lb.is_ok(), lb.is_err() ? lb.error().c_str() : "");
        const LeaderboardRow* lrow = find_row(lb.value(), sid);
        QVERIFY(lrow);
        QCOMPARE(lrow->resolved, 3);
        QVERIFY(qAbs(lrow->net_pnl - 1.60) < 1e-9);
        QVERIFY(qAbs(lrow->hit_rate - (2.0 / 3.0)) < 1e-9);
        QVERIFY(qAbs(lrow->max_drawdown - 1.10) < 1e-9);
        QVERIFY(qAbs(lrow->gross_notional - 300.0) < 1e-9);
        QCOMPARE(lrow->degraded, 1);
        QCOMPARE(lrow->data_gap_count, 1);
        QCOMPARE(lrow->unknown_count, 0);
        QVERIFY(!lrow->hypothetical);
    }

    // (b) An 'unknown' data_quality row must count toward unknown_count, NOT
    // degraded_count -- 'unknown' means no freshness telemetry at all, which
    // is not evidence of degradation (PaperExecutor.h).
    void unknown_data_quality_counts_separately_from_degraded() {
        auto strat = register_strategy(QStringLiteral("spot"), QStringLiteral("BTC-USD"),
                                       QJsonObject{{"case", "b"}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        const qint64 day0 = day_anchor_ms(1);
        insert_position(QStringLiteral("scorer-b-1"), sid, QStringLiteral("closed"), 1.0, 50.0,
                        QStringLiteral("degraded"), day0 + 1000, QStringLiteral("target"), day0 + 1000);
        insert_position(QStringLiteral("scorer-b-2"), sid, QStringLiteral("closed"), 1.0, 50.0,
                        QStringLiteral("unknown"), day0 + 2000, QStringLiteral("target"), day0 + 2000);

        auto rep = score_all(QStringLiteral("default"), day0 + 10000);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");

        const ScoreRow row = fetch_score(sid, day_string(day0));
        QVERIFY(row.found);
        QCOMPARE(row.degraded_count, 1);

        auto lb = leaderboard(QStringLiteral("default"));
        QVERIFY2(lb.is_ok(), lb.is_err() ? lb.error().c_str() : "");
        const LeaderboardRow* lrow = find_row(lb.value(), sid);
        QVERIFY(lrow);
        QCOMPARE(lrow->degraded, 1);
        QCOMPARE(lrow->unknown_count, 1);
    }

    // (c) score_all must be idempotent: calling it twice with no new
    // activity between calls writes byte-identical sandbox_score rows.
    void score_all_is_idempotent() {
        auto strat = register_strategy(QStringLiteral("spot"), QStringLiteral("BTC-USD"),
                                       QJsonObject{{"case", "c"}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        const qint64 day0 = day_anchor_ms(2);
        insert_position(QStringLiteral("scorer-c-1"), sid, QStringLiteral("closed"), 3.0, 100.0,
                        QStringLiteral("ok"), day0 + 1000, QStringLiteral("target"), day0 + 1000);

        const qint64 now_ms = day0 + 10000;
        auto rep1 = score_all(QStringLiteral("default"), now_ms);
        QVERIFY2(rep1.is_ok(), rep1.is_err() ? rep1.error().c_str() : "");
        const ScoreRow first = fetch_score(sid, day_string(day0));
        QVERIFY(first.found);

        auto rep2 = score_all(QStringLiteral("default"), now_ms);
        QVERIFY2(rep2.is_ok(), rep2.is_err() ? rep2.error().c_str() : "");
        const ScoreRow second = fetch_score(sid, day_string(day0));
        QVERIFY(second.found);

        QCOMPARE(second.resolved_count, first.resolved_count);
        QVERIFY(qAbs(second.net_pnl - first.net_pnl) < 1e-12);
        QVERIFY(qAbs(second.hit_rate - first.hit_rate) < 1e-12);
        QVERIFY(qAbs(second.avg_win - first.avg_win) < 1e-12);
        QVERIFY(qAbs(second.avg_loss - first.avg_loss) < 1e-12);
        QVERIFY(qAbs(second.max_drawdown - first.max_drawdown) < 1e-12);
        QCOMPARE(second.degraded_count, first.degraded_count);
        QCOMPARE(second.unfilled_count, first.unfilled_count);
        QCOMPARE(second.open_count, first.open_count);
        QVERIFY(qAbs(second.gross_notional - first.gross_notional) < 1e-12);
    }

    // (d) hypothetical (LeaderboardRow::hypothetical) is read from the
    // strategy's own params_json, matching the exact key PaperExecutor.cpp's
    // run_cycle branches on.
    void leaderboard_reads_hypothetical_from_strategy_params() {
        auto strat = register_strategy(QStringLiteral("long_short"), QStringLiteral("XHY-USD"),
                                       QJsonObject{{"hypothetical", true}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        auto rep = score_all(QStringLiteral("default"), day_anchor_ms(3));
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");

        auto lb = leaderboard(QStringLiteral("default"));
        QVERIFY2(lb.is_ok(), lb.is_err() ? lb.error().c_str() : "");
        const LeaderboardRow* lrow = find_row(lb.value(), sid);
        QVERIFY(lrow);
        QVERIFY(lrow->hypothetical);
        QCOMPARE(lrow->kind, QStringLiteral("long_short"));
    }

    // (e) T9-review fold: max_drawdown must be zeroed on every score_all-
    // upserted row whose score_date is NOT the latest day THIS CALL touches
    // -- not just "never written" on old rows, but actively re-zeroed each
    // call, since load_activity rescans full history and re-buckets every
    // day that ever had activity. A first call scores day0 (the latest day
    // at that time) with its real drawdown; a second call two days later,
    // with zero new activity, must re-touch day0 (still reachable via full-
    // history rescan) and overwrite its drawdown to 0 now that day0 is no
    // longer the latest day, while the new latest day (today of the second
    // call) gets the (unchanged) real drawdown.
    void score_all_zeros_drawdown_on_non_latest_day_across_calls() {
        auto strat = register_strategy(QStringLiteral("spot"), QStringLiteral("BTC-USD"),
                                       QJsonObject{{"case", "e"}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        const qint64 day0 = day_anchor_ms(4);
        // +5.0 then -3.0: cumulative peak 5.0, trough 2.0 -> drawdown 3.0.
        insert_position(QStringLiteral("scorer-e-1"), sid, QStringLiteral("closed"), 5.0, 100.0,
                        QStringLiteral("ok"), day0 + 1000, QStringLiteral("target"), day0 + 1000);
        insert_position(QStringLiteral("scorer-e-2"), sid, QStringLiteral("closed"), -3.0, 100.0,
                        QStringLiteral("ok"), day0 + 2000, QStringLiteral("stop"), day0 + 2000);

        auto rep1 = score_all(QStringLiteral("default"), day0 + 10000); // still day0 -> day0 is latest.
        QVERIFY2(rep1.is_ok(), rep1.is_err() ? rep1.error().c_str() : "");
        const ScoreRow first = fetch_score(sid, day_string(day0));
        QVERIFY(first.found);
        QVERIFY2(qAbs(first.max_drawdown - 3.0) < 1e-9, "day0 is the latest touched day on the first call");

        const qint64 day2 = day_anchor_ms(6); // two days later, no new activity in between.
        auto rep2 = score_all(QStringLiteral("default"), day2 + 10000);
        QVERIFY2(rep2.is_ok(), rep2.is_err() ? rep2.error().c_str() : "");

        const ScoreRow day0_after = fetch_score(sid, day_string(day0));
        QVERIFY(day0_after.found);
        QVERIFY2(qAbs(day0_after.max_drawdown - 0.0) < 1e-9,
                 "day0's drawdown must be re-zeroed once it is no longer the latest touched day");

        const ScoreRow day2_row = fetch_score(sid, day_string(day2));
        QVERIFY(day2_row.found);
        QVERIFY2(qAbs(day2_row.max_drawdown - 3.0) < 1e-9,
                 "the new latest day must carry the (unchanged) full-history drawdown");
    }

    // (f) T9-review fold: realized_pnl == 0 is a resolved round trip (not a
    // data-gap -- has_pnl is true), counts toward resolved_count/net_pnl/
    // hit_rate's denominator, but is neither a win nor a loss: it must not
    // land in avg_win or avg_loss.
    void realized_pnl_zero_counts_resolved_but_not_win_or_loss() {
        auto strat = register_strategy(QStringLiteral("spot"), QStringLiteral("BTC-USD"),
                                       QJsonObject{{"case", "f"}});
        QVERIFY(strat.is_ok());
        const QString sid = strat.value();

        const qint64 day0 = day_anchor_ms(5);
        insert_position(QStringLiteral("scorer-f-1"), sid, QStringLiteral("closed"), 2.0, 100.0,
                        QStringLiteral("ok"), day0 + 1000, QStringLiteral("target"), day0 + 1000);
        insert_position(QStringLiteral("scorer-f-2"), sid, QStringLiteral("closed"), -1.0, 100.0,
                        QStringLiteral("ok"), day0 + 2000, QStringLiteral("stop"), day0 + 2000);
        insert_position(QStringLiteral("scorer-f-3"), sid, QStringLiteral("closed"), 0.0, 100.0,
                        QStringLiteral("ok"), day0 + 3000, QStringLiteral("target"), day0 + 3000);

        auto rep = score_all(QStringLiteral("default"), day0 + 10000);
        QVERIFY2(rep.is_ok(), rep.is_err() ? rep.error().c_str() : "");

        const ScoreRow row = fetch_score(sid, day_string(day0));
        QVERIFY(row.found);
        QCOMPARE(row.resolved_count, 3);
        QVERIFY2(qAbs(row.net_pnl - 1.0) < 1e-9, "the flat 0.0 row still contributes 0 to net_pnl");
        QVERIFY2(qAbs(row.hit_rate - (1.0 / 3.0)) < 1e-9, "the flat row is not a win -- hit_rate denominator is 3");
        QVERIFY2(qAbs(row.avg_win - 2.0) < 1e-9, "avg_win must be unaffected by the flat row");
        QVERIFY2(qAbs(row.avg_loss - (-1.0)) < 1e-9, "avg_loss must be unaffected by the flat row");

        auto lb = leaderboard(QStringLiteral("default"));
        QVERIFY2(lb.is_ok(), lb.is_err() ? lb.error().c_str() : "");
        const LeaderboardRow* lrow = find_row(lb.value(), sid);
        QVERIFY(lrow);
        QCOMPARE(lrow->resolved, 3);
        QVERIFY(qAbs(lrow->net_pnl - 1.0) < 1e-9);
    }

    // Phase 1: lane significance clusters by session and gates on conservative
    // expectancy (mean - 2 clustered SE), the same don't-fool-yourself bar as
    // the Kalshi gate. Pure -- no DB.
    void evaluate_lane_significance_clusters_by_session_and_gates_on_expectancy() {
        QVector<LanePnlSample> samples;
        // Lane A: a genuine edge -- +10/session (3 trades of 4,4,2) for 30
        // sessions. Consistent => conservative expectancy > 0.
        for (int d = 0; d < 30; ++d) {
            const QString s = QStringLiteral("A-day-%1").arg(d);
            samples.append({QStringLiteral("A"), s, 4.0});
            samples.append({QStringLiteral("A"), s, 4.0});
            samples.append({QStringLiteral("A"), s, 2.0});
        }
        // Lane B: no edge -- session totals alternate +30/-30, mean 0, high
        // variance => conservative <= 0.
        for (int d = 0; d < 30; ++d)
            samples.append({QStringLiteral("B"), QStringLiteral("B-day-%1").arg(d),
                            d % 2 ? 30.0 : -30.0});
        // Lane C: thin -- only 5 independent sessions.
        for (int d = 0; d < 5; ++d)
            samples.append({QStringLiteral("C"), QStringLiteral("C-day-%1").arg(d), 100.0});

        const auto result = evaluate_lane_significance(samples, 20);
        QCOMPARE(result.size(), 3);  // grouping keeps the three lanes distinct
        const LaneSignificance& A = result[0];  // sorted by lane
        const LaneSignificance& B = result[1];
        const LaneSignificance& C = result[2];

        QCOMPARE(A.lane, QStringLiteral("A"));
        QCOMPARE(A.trades, 90);
        QCOMPARE(A.sessions, 30);
        QVERIFY(qAbs(A.mean_session_pnl - 10.0) < 1e-9);
        QVERIFY(A.ready);
        QVERIFY(A.has_edge);
        QVERIFY(A.conservative_expectancy > 0.0);

        QVERIFY(B.ready);
        QVERIFY(!B.has_edge);
        QVERIFY(B.conservative_expectancy <= 0.0);

        QVERIFY(!C.ready);          // < 20 sessions
        QVERIFY(!C.has_edge);
    }

    void evaluate_lane_significance_requires_variance_estimating_sample() {
        const QVector<LanePnlSample> samples{
            {QStringLiteral("A"), QStringLiteral("only-session"), 100.0},
        };

        const auto result = evaluate_lane_significance(samples, 1);
        QCOMPARE(result.size(), 1);
        QCOMPARE(result[0].sessions, 1);
        QVERIFY(!result[0].ready);
        QVERIFY(!result[0].has_edge);
        QVERIFY(result[0].reason.contains(QStringLiteral("1 < 2")));
    }
};

QTEST_GUILESS_MAIN(TstSandboxScorer)
#include "tst_sandbox_scorer.moc"
