// WHAT THE RECORD TEACHES — the edge-autopsy lessons card (issue #174).
//
// The card renders conclusions that future bets are supposed to lean on, so
// the ways it can lie are specific:
//
//   1. a lesson from 25 settled positions rendering as though it were as
//      strong as one from 271 contracts — so EVERY line carries its sample
//      size, and a lesson that states none says SAMPLE SIZE NOT STATED rather
//      than going blank or showing a zero;
//   2. a stale artifact reading as current — so undated, future-dated and
//      too-old all classify stale, and a stale report cannot paint a single
//      line green however good its verdicts are;
//   3. an absent artifact rendering as an empty card ("nothing to learn") —
//      so no file yields exactly one refusal line with no digits anywhere;
//   4. a verdict word this build does not know falling through to a colour
//      that reads as normal.
//
// The freshness classifier is the piece a neuter check targets: widen the
// threshold or drop the future-dated branch and the stale tests below must go
// red, or they are not testing what they claim to.

#include "services/prediction/kalshi/KalshiBotRuntime.h"
#include "services/prediction/kalshi/KalshiEdgeLessons.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using openmarketterminal::services::prediction::kalshi_ns::kalshi_edge_lessons;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_edge_read_report_file;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_edge_report_freshness;
using openmarketterminal::services::prediction::kalshi_ns::KalshiEdgeLessonsView;
using openmarketterminal::services::prediction::kalshi_ns::KalshiEdgeReportFile;
using openmarketterminal::services::prediction::kalshi_ns::kKalshiEdgeReportCadenceMs;
using openmarketterminal::services::prediction::kalshi_ns::kKalshiEdgeReportStaleMs;

namespace {

constexpr qint64 kNow = 1'785'000'000'000LL;
constexpr qint64 kDay = 24 * 3'600'000LL;

QJsonObject number(const QString& label, double value, const QString& text) {
    return QJsonObject{{QStringLiteral("label"), label},
                       {QStringLiteral("value"), value},
                       {QStringLiteral("text"), text}};
}

QJsonObject lesson(const QString& id, const QString& verdict, int n, const QString& unit) {
    QJsonObject sample{{QStringLiteral("unit"), unit}};
    if (n >= 0) sample.insert(QStringLiteral("n"), n);
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), QStringLiteral("A TITLE")},
        {QStringLiteral("verdict"), verdict},
        {QStringLiteral("claim"), QStringLiteral("the claim")},
        {QStringLiteral("verdict_reason"), QStringLiteral("because the numbers say so")},
        {QStringLiteral("key_numbers"), QJsonArray{number(QStringLiteral("drift"), 0.0122,
                                                          QStringLiteral("+1.22¢"))}},
        {QStringLiteral("sample"), sample},
        {QStringLiteral("data_span"), QJsonObject{{QStringLiteral("text"),
                                                   QStringLiteral("9.0h of record")}}}};
}

/// A published artifact, `age_ms` old relative to kNow.
QJsonObject report(qint64 age_ms, const QJsonArray& lessons) {
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QStringLiteral("kalshi_edge_report")},
                       {QStringLiteral("generated_at_ms"), static_cast<double>(kNow - age_ms)},
                       {QStringLiteral("lessons"), lessons}};
}

KalshiEdgeReportFile published(const QJsonObject& object) {
    KalshiEdgeReportFile file;
    file.available = true;
    file.object = object;
    return file;
}

bool has_digit(const QString& text) {
    for (const QChar c : text)
        if (c.isDigit()) return true;
    return false;
}

QJsonArray four_lessons() {
    return QJsonArray{lesson(QStringLiteral("Q1"), QStringLiteral("FEE_EATEN"), 22,
                             QStringLiteral("spot moves at 2.0σ")),
                      lesson(QStringLiteral("Q2"), QStringLiteral("NO_EDGE"), 271,
                             QStringLiteral("settled contracts")),
                      lesson(QStringLiteral("Q3"), QStringLiteral("EDGE"), 251,
                             QStringLiteral("out-of-sample contracts")),
                      lesson(QStringLiteral("Q4"), QStringLiteral("INSUFFICIENT_DATA"), 25,
                             QStringLiteral("settled positions"))};
}

} // namespace

class TestKalshiEdgeLessons : public QObject {
    Q_OBJECT

  private slots:

    // ── rule 3: no artifact means no card, and no digits ──────────────────
    void absent_file_renders_one_refusal_line_and_no_numbers() {
        const KalshiEdgeLessonsView view = kalshi_edge_lessons(KalshiEdgeReportFile{}, kNow);
        QVERIFY(!view.available);
        QCOMPARE(view.lessons.size(), 0);
        QCOMPARE(view.lines().size(), 1);
        QVERIFY(view.header.startsWith(QStringLiteral("WHAT THE RECORD TEACHES · UNAVAILABLE")));
        QVERIFY(view.header.contains(QStringLiteral("kalshi-edge-report.json")));
        // The reason may name the file; it must claim no measurement.
        QVERIFY(!view.header.contains(QStringLiteral("n=")));
    }

    void an_unparseable_artifact_is_unavailable_rather_than_empty() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-edge-report.json"));
        QFile handle(path);
        QVERIFY(handle.open(QIODevice::WriteOnly | QIODevice::Text));
        handle.write("{ this is not json");
        handle.close();

        const KalshiEdgeReportFile file = kalshi_edge_read_report_file(path);
        QVERIFY(!file.available);
        QVERIFY(file.why.contains(QStringLiteral("not parseable")));
        const KalshiEdgeLessonsView view = kalshi_edge_lessons(file, kNow);
        QVERIFY(!view.available);
        QCOMPARE(view.lines().size(), 1);
        QVERIFY(!has_digit(view.lines().first().section(QStringLiteral(".json"), 1)));
    }

    void a_missing_file_reads_missing_through_the_real_reader() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const KalshiEdgeReportFile file =
            kalshi_edge_read_report_file(dir.filePath(QStringLiteral("nope.json")));
        QVERIFY(!file.available);
        QVERIFY(file.why.contains(QStringLiteral("has not published")));
    }

    // ── rule 1: the sample size is on every line, always ──────────────────
    void every_line_carries_its_sample_size_in_its_own_unit() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        QCOMPARE(view.lessons.size(), 4);
        for (const auto& rendered : view.lessons)
            QVERIFY2(rendered.text.contains(QStringLiteral("n=")) ||
                         rendered.text.contains(QStringLiteral("SAMPLE SIZE NOT STATED")),
                     qUtf8Printable(rendered.text));
        QVERIFY(view.lessons.at(0).text.contains(QStringLiteral("n=22 spot moves")));
        QVERIFY(view.lessons.at(3).text.contains(QStringLiteral("n=25 settled positions")));
        // A 25-position lesson and a 271-contract lesson state DIFFERENT units:
        // the card never flattens them into one denominator.
        QVERIFY(view.lessons.at(1).text.contains(QStringLiteral("n=271 settled contracts")));
    }

    /// The cockpit draws one line per lesson in a fixed-width scene and elides
    /// from the right — which is where the sample size is. The compact form
    /// exists so that surface still carries it, so it must front-load the
    /// sample and be short enough to be worth choosing.
    void the_compact_line_keeps_the_sample_size_a_narrow_surface_would_cut() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        for (const auto& rendered : view.lessons) {
            QVERIFY2(rendered.compact.contains(QStringLiteral("n=")),
                     qUtf8Printable(rendered.compact));
            QVERIFY2(rendered.compact.size() < rendered.text.size(),
                     qUtf8Printable(rendered.compact));
            // The verdict survives too — a compact line that dropped it would
            // be a sample size attached to nothing.
            QVERIFY(rendered.compact.contains(rendered.verdict));
            // The sample must sit ahead of the span, so elision eats the span
            // first and never the denominator.
            QVERIFY(rendered.compact.indexOf(QStringLiteral("n=")) <
                    rendered.compact.indexOf(QStringLiteral("of record")));
        }
        QVERIFY(view.lessons.at(3).compact.contains(QStringLiteral("n=25 settled positions")));
    }

    void a_stale_compact_line_still_carries_the_age() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(30 * kDay, four_lessons())), kNow);
        for (const auto& rendered : view.lessons)
            QVERIFY2(rendered.compact.contains(QStringLiteral("STALE")),
                     qUtf8Printable(rendered.compact));
    }

    void a_lesson_that_states_no_sample_size_says_so_rather_than_showing_zero() {
        const QJsonArray lessons{lesson(QStringLiteral("Q1"),
                                        QStringLiteral("INSUFFICIENT_DATA"), -1,
                                        QStringLiteral("spot moves"))};
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, lessons)), kNow);
        QCOMPARE(view.lessons.size(), 1);
        QVERIFY(view.lessons.first().text.contains(QStringLiteral("SAMPLE SIZE NOT STATED")));
        QVERIFY(!view.lessons.first().text.contains(QStringLiteral("n=0")));
    }

    void an_insufficient_data_lesson_renders_its_verdict_and_stays_grey() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        const auto& q4 = view.lessons.at(3);
        QCOMPARE(q4.verdict, QStringLiteral("INSUFFICIENT_DATA"));
        QCOMPARE(q4.role, QStringLiteral("grey"));
        QVERIFY(q4.text.contains(QStringLiteral("INSUFFICIENT_DATA")));
    }

    void a_span_the_report_did_not_state_reads_not_stated() {
        QJsonObject bare = lesson(QStringLiteral("Q1"), QStringLiteral("NO_EDGE"), 9,
                                  QStringLiteral("spot moves"));
        bare.remove(QStringLiteral("data_span"));
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, QJsonArray{bare})), kNow);
        QVERIFY(view.lessons.first().text.contains(QStringLiteral("span NOT STATED")));
    }

    void key_numbers_are_printed_as_the_driver_formatted_them() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        // The driver's text, not a re-rounding of the raw value beside it.
        QVERIFY(view.lessons.first().text.contains(QStringLiteral("drift +1.22")));
        QVERIFY(!view.lessons.first().text.contains(QStringLiteral("0.0122")));
    }

    // ── rule 2 / 4: verdict colours, and the unknown word ─────────────────
    void each_verdict_maps_to_its_own_colour_intent() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        QCOMPARE(view.lessons.at(0).role, QStringLiteral("amber"));  // FEE_EATEN
        QCOMPARE(view.lessons.at(1).role, QStringLiteral("grey"));   // NO_EDGE
        QCOMPARE(view.lessons.at(2).role, QStringLiteral("green"));  // EDGE, fresh
        QCOMPARE(view.lessons.at(3).role, QStringLiteral("grey"));   // INSUFFICIENT_DATA
        QCOMPARE(view.header_role, QStringLiteral("cyan"));
        QVERIFY(!view.stale);
    }

    void a_verdict_this_build_does_not_know_is_shown_verbatim_and_warned_on() {
        const QJsonArray lessons{lesson(QStringLiteral("Q9"), QStringLiteral("SOMETHING_NEW"), 5,
                                        QStringLiteral("widgets"))};
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, lessons)), kNow);
        QCOMPARE(view.lessons.first().verdict, QStringLiteral("SOMETHING_NEW"));
        QVERIFY(view.lessons.first().text.contains(QStringLiteral("SOMETHING_NEW")));
        // Not green, and not silently normal.
        QCOMPARE(view.lessons.first().role, QStringLiteral("amber"));
    }

    void a_report_with_no_lessons_says_nothing_was_measured() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, QJsonArray{})), kNow);
        QVERIFY(view.available);
        QCOMPARE(view.lessons.size(), 0);
        QVERIFY(view.header.contains(QStringLiteral("NO lesson")));
    }

    // ── rule 3: staleness outranks a good verdict ─────────────────────────
    void a_stale_report_demotes_every_green_line_to_amber() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kKalshiEdgeReportStaleMs + 1, four_lessons())),
                                kNow);
        QVERIFY(view.stale);
        QCOMPARE(view.header_role, QStringLiteral("amber"));
        // The EDGE verdict is unchanged — only its colour is.
        QCOMPARE(view.lessons.at(2).verdict, QStringLiteral("EDGE"));
        QCOMPARE(view.lessons.at(2).role, QStringLiteral("amber"));
        for (const auto& rendered : view.lessons)
            QVERIFY(rendered.role != QStringLiteral("green"));
    }

    void a_stale_report_carries_its_age_on_every_line_not_only_the_header() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kKalshiEdgeReportStaleMs + 1, four_lessons())),
                                kNow);
        for (const auto& line : view.lines())
            QVERIFY2(line.contains(QStringLiteral("STALE")), qUtf8Printable(line));
    }

    void a_fresh_report_does_not_shout_stale_on_every_line() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(kDay, four_lessons())), kNow);
        for (const auto& rendered : view.lessons)
            QVERIFY(!rendered.text.contains(QStringLiteral("STALE")));
        // Under two days the age is still stated in hours — "24h ago" is a
        // sharper reading of a day-old artifact than "1d ago".
        QVERIFY2(view.header.contains(QStringLiteral("generated 24h ago")),
                 qUtf8Printable(view.header));
    }

    // ── the freshness classifier itself ───────────────────────────────────
    void a_single_missed_weekly_run_still_reads_fresh() {
        // The threshold's whole justification: one miss leaves the artifact
        // ~14 days old and must NOT flap to stale.
        const auto freshness =
            kalshi_edge_report_freshness(report(2 * kKalshiEdgeReportCadenceMs, {}), kNow);
        QVERIFY(freshness.dated);
        QVERIFY(!freshness.stale);
    }

    void the_stale_boundary_is_exact() {
        QVERIFY(!kalshi_edge_report_freshness(report(kKalshiEdgeReportStaleMs, {}), kNow).stale);
        QVERIFY(kalshi_edge_report_freshness(report(kKalshiEdgeReportStaleMs + 1, {}), kNow).stale);
    }

    /// Pinned in ABSOLUTE days, deliberately. Every other staleness assertion
    /// here is written against `kKalshiEdgeReportStaleMs`, so widening that
    /// constant moves the tests with it and they all keep passing — which is
    /// exactly what a neuter check found. These two do not move: they say what
    /// the threshold must be worth in days, not that it equals itself.
    void the_threshold_is_pinned_in_days_not_to_its_own_constant() {
        QVERIFY2(!kalshi_edge_report_freshness(report(14 * kDay, {}), kNow).stale,
                 "a fortnight-old report (one missed weekly run) must still read fresh");
        QVERIFY2(kalshi_edge_report_freshness(report(16 * kDay, {}), kNow).stale,
                 "a report older than two cadences plus slack must read stale");
        QVERIFY2(kalshi_edge_report_freshness(report(30 * kDay, {}), kNow).stale,
                 "a month-old report must read stale");
    }

    void a_month_old_report_cannot_paint_a_green_line() {
        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(30 * kDay, four_lessons())), kNow);
        QVERIFY(view.stale);
        QCOMPARE(view.header_role, QStringLiteral("amber"));
        QCOMPARE(view.lessons.at(2).verdict, QStringLiteral("EDGE"));
        QCOMPARE(view.lessons.at(2).role, QStringLiteral("amber"));
        for (const QString& line : view.lines())
            QVERIFY2(line.contains(QStringLiteral("STALE")), qUtf8Printable(line));
    }

    void an_undated_report_is_stale_never_current() {
        QJsonObject undated = report(kDay, four_lessons());
        undated.remove(QStringLiteral("generated_at_ms"));
        const auto freshness = kalshi_edge_report_freshness(undated, kNow);
        QVERIFY(!freshness.dated);
        QVERIFY(freshness.stale);
        QCOMPARE(freshness.age_ms, -1LL);
        QVERIFY(freshness.text.contains(QStringLiteral("UNKNOWN time")));

        const KalshiEdgeLessonsView view = kalshi_edge_lessons(published(undated), kNow);
        QVERIFY(view.stale);
        QCOMPARE(view.lessons.at(2).role, QStringLiteral("amber"));  // the EDGE line
    }

    void a_future_dated_report_is_mistrusted_rather_than_clamped() {
        const auto freshness = kalshi_edge_report_freshness(report(-kDay, four_lessons()), kNow);
        QVERIFY(!freshness.dated);
        QVERIFY(freshness.stale);
        QVERIFY(freshness.text.contains(QStringLiteral("FUTURE")));

        const KalshiEdgeLessonsView view =
            kalshi_edge_lessons(published(report(-kDay, four_lessons())), kNow);
        QVERIFY(view.stale);
        for (const auto& rendered : view.lessons)
            QVERIFY(rendered.role != QStringLiteral("green"));
    }

    void the_weekly_age_text_speaks_in_days_not_hundreds_of_hours() {
        const auto freshness = kalshi_edge_report_freshness(report(21 * kDay, {}), kNow);
        QVERIFY2(freshness.text.contains(QStringLiteral("21d ago")),
                 qUtf8Printable(freshness.text));
        QVERIFY(!freshness.text.contains(QStringLiteral("504h")));
    }

    void the_display_threshold_is_not_the_bot_loops() {
        // Reusing the minute-scale loop threshold would paint a healthy weekly
        // artifact amber minutes after it was written.
        QVERIFY(kKalshiEdgeReportStaleMs >
                openmarketterminal::services::prediction::kalshi_ns::kKalshiBotStaleMs);
        QVERIFY(kKalshiEdgeReportStaleMs > 2 * kKalshiEdgeReportCadenceMs);
    }
};

QTEST_MAIN(TestKalshiEdgeLessons)
#include "tst_kalshi_edge_lessons.moc"
