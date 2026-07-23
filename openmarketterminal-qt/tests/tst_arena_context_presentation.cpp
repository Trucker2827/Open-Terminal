#include <QtTest>

#include "screens/kalshi/ArenaContextPresentation.h"

using namespace openmarketterminal::screens::kalshi;

namespace {

QJsonObject lane(const QString& id, bool comparable, double coverage,
                 double brier = 0.0, int rank = 0, const QString& reason = {}) {
    QJsonObject object{{"id", id}, {"comparable", comparable}, {"coverage", coverage},
                       {"brier", brier}};
    if (rank > 0) object.insert("rank", rank);
    if (!reason.isEmpty()) object.insert("reason", reason);
    return object;
}

} // namespace

class ArenaContextPresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void missing_report_reads_offline() {
        const auto view = present_arena_context({}, 1'000'000);
        QVERIFY(view.offline);
        QVERIFY(view.headline.contains("ARENA OFFLINE"));
        QVERIFY(view.headline.contains("arena-report.json"));
    }

    void insufficient_data_verdict_is_shown_verbatim() {
        const qint64 now = 2'000'000'000;
        const QJsonObject report{{"verdict", "INSUFFICIENT_DATA"}, {"rounds_total", 69},
            {"generated_at_ms", double(now - 60'000)},
            {"leaderboard", QJsonArray{lane("qwen3-local", false, 0.39, 0.4848, 0,
                                            "coverage 39% below 80% floor")}}};
        const auto view = present_arena_context(report, now);
        QVERIFY(!view.offline);
        QVERIFY(!view.stale);
        QVERIFY(!view.leader);
        QVERIFY(view.headline.contains("ARENA INSUFFICIENT_DATA"));
        QVERIFY(view.headline.contains("69 rounds"));
        QVERIFY(view.headline.contains("60s ago"));
    }

    void top_comparable_lane_is_lowest_rank() {
        const qint64 now = 3'000'000'000;
        const QJsonObject report{{"verdict", "LEADER: coney-llama3.1"}, {"rounds_total", 120},
            {"generated_at_ms", double(now - 30'000)},
            {"leaderboard", QJsonArray{
                lane("runner-up", true, 0.92, 0.2101, 2),
                lane("coney-llama3.1", true, 0.88, 0.1987, 1),
                lane("qwen3-local", false, 0.39, 0.4848, 0,
                     "coverage 39% below 80% floor")}}};
        const auto view = present_arena_context(report, now);
        QVERIFY(view.leader);
        QVERIFY(view.lane.contains("TOP LANE coney-llama3.1"));
        QVERIFY(view.lane.contains("BRIER 0.1987"));
        QVERIFY(view.lane.contains("COVERAGE 88%"));
        QVERIFY(!view.lane.contains("runner-up"));
    }

    void none_comparable_shows_honest_reason_of_closest_lane() {
        const qint64 now = 4'000'000'000;
        const QJsonObject report{{"verdict", "INSUFFICIENT_DATA"}, {"rounds_total", 69},
            {"generated_at_ms", double(now - 5'000)},
            {"leaderboard", QJsonArray{
                lane("qwen3.5-local", false, 0.0, 0.0, 0, "coverage 0% below 80% floor"),
                lane("coney-llama3.1", false, 0.59, 0.2489, 0,
                     "coverage 59% below 80% floor")}}};
        const auto view = present_arena_context(report, now);
        QVERIFY(view.lane.contains("NO COMPARABLE LANE"));
        QVERIFY(view.lane.contains("coney-llama3.1"));
        QVERIFY(view.lane.contains("coverage 59% below 80% floor"));
    }

    void old_report_reads_stale() {
        const qint64 now = 5'000'000'000;
        const QJsonObject report{{"verdict", "INSUFFICIENT_DATA"}, {"rounds_total", 12},
            {"generated_at_ms", double(now - kArenaReportStaleMs - 1)},
            {"leaderboard", QJsonArray{}}};
        const auto view = present_arena_context(report, now);
        QVERIFY(!view.offline);
        QVERIFY(view.stale);
        QVERIFY(view.headline.contains("ARENA STALE"));
        QVERIFY(view.headline.contains("INSUFFICIENT_DATA"));
        QVERIFY(view.lane.contains("NO LANES REPORTED YET"));
    }

    void missing_timestamp_reads_stale_not_fresh() {
        const QJsonObject report{{"verdict", "INSUFFICIENT_DATA"}, {"rounds_total", 3}};
        const auto view = present_arena_context(report, 6'000'000'000);
        QVERIFY(view.stale);
        QVERIFY(view.headline.contains("age unknown"));
    }
};

QTEST_GUILESS_MAIN(ArenaContextPresentationTest)
#include "tst_arena_context_presentation.moc"
