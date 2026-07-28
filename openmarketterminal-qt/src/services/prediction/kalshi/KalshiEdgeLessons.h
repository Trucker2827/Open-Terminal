#pragma once

// WHAT THE RECORD TEACHES — the edge-autopsy lessons card (issue #174).
//
// Why this module exists: the #169 autopsy measured where edge actually is and
// where it is not, and those conclusions ended up in a markdown report. A
// report is a snapshot of one afternoon. The lessons future bets depend on —
// "the calibrator is uninformative, not mistuned", "the quote lag is real but
// fee-sized", "the bot's own record is 25 positions against a gate of 300" —
// belong where bets are made, dated and sample-sized, and they have to keep
// being true rather than keep being quoted.
//
// The artifact is `kalshi-edge-report.json`, published by
// scripts/research/kalshi_edge_report.py (weekly job, or `kalshi bot lessons
// --refresh`) over #169's own scripts. This header is the ONE reader and the
// ONE formatter over it, header-inline for the same reason KalshiBotFunnel.h
// is: the GUI does not link the CLI's translation units, so a shared header is
// what makes "the BOT tab, the cockpit and `kalshi bot lessons` show the same
// sentences" a property rather than a claim.
//
// Four honesty rules are structural here:
//
//   1. **Every line carries its sample size, in the unit that question
//      measures in.** A lesson from 25 settled positions must not look like a
//      lesson from 271 contracts. A lesson whose payload states no sample size
//      says SAMPLE SIZE NOT STATED — it does not get a blank, and it certainly
//      does not get a zero.
//   2. **Nothing is computed here.** The verdict, the claim, the numbers and
//      the span all come from the artifact exactly as the driver derived them.
//      A verdict word this build does not recognise is rendered verbatim and
//      painted as a warning, never silently mapped to something familiar.
//   3. **Staleness cannot be outvoted by a good verdict.** A stale artifact
//      demotes every green line to amber, for the same reason a stale gate
//      PASS is amber (issue #167): the verdict may be true of a record that
//      has since moved. Undated and future-dated both count as stale.
//   4. **An unavailable artifact renders exactly one line — the refusal and
//      its reason — with no numbers anywhere.** No file means no lessons, not
//      an empty card that reads as "nothing to learn".
//
// This module is DISPLAY ONLY. Nothing here is read by KalshiBotDecision, the
// sealed gate, or the live permission path: a lesson becomes strategy only
// through its own issue and review, as #165 did.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Where the driver publishes the artifact. Named here beside the readers, so
/// a rename cannot leave one surface on the old path.
inline constexpr auto kKalshiEdgeReportFile = "kalshi-edge-report.json";

inline constexpr qint64 kKalshiEdgeDayMs = 24LL * 3'600'000;

/// The scheduled cadence, matching org.openterminal.kalshi-edge-report.plist.
/// Named after the cadence rather than after a duration, so the plist and this
/// threshold are obviously the same fact stated twice.
inline constexpr qint64 kKalshiEdgeReportCadenceMs = 7 * kKalshiEdgeDayMs;

/// When the card stops vouching for the artifact's age: two cadences plus a
/// day. The arithmetic is deliberate — one MISSED weekly run leaves the newest
/// artifact ~14 days old, and a threshold of exactly two cadences would make a
/// single miss flap across the boundary depending on the hour the job ran. So
/// a single missed run still reads fresh, and two consecutive misses do not.
///
/// Deliberately not kKalshiBotStaleMs (two 60s tick intervals) or
/// kKalshiBotGateStaleMs: those ask whether a loop that runs every minute is
/// still running. This asks whether a WEEKLY research job is still running,
/// and reusing a minute-scale threshold would paint a healthy artifact amber
/// nine minutes after it was written.
inline constexpr qint64 kKalshiEdgeReportStaleMs =
    (2 * kKalshiEdgeReportCadenceMs) + kKalshiEdgeDayMs;

/// The verdict vocabulary the driver publishes. Mirrored here so an unknown
/// word can be RECOGNISED as unknown rather than falling through to a default
/// colour that reads as normal.
inline constexpr auto kKalshiEdgeVerdictEdge = "EDGE";
inline constexpr auto kKalshiEdgeVerdictNoEdge = "NO_EDGE";
inline constexpr auto kKalshiEdgeVerdictFeeEaten = "FEE_EATEN";
inline constexpr auto kKalshiEdgeVerdictInsufficient = "INSUFFICIENT_DATA";

/// An age a human reads over WEEKS. `kalshi_bot_age_text` tops out at hours,
/// which is right for a 60-second loop and useless for a weekly artifact: a
/// three-week-old report would render "512h ago".
inline QString kalshi_edge_age_text(qint64 age_ms) {
    if (age_ms < 3'600'000) return QStringLiteral("%1m ago").arg(age_ms / 60'000);
    if (age_ms < 2 * kKalshiEdgeDayMs) return QStringLiteral("%1h ago").arg(age_ms / 3'600'000);
    return QStringLiteral("%1d ago").arg(age_ms / kKalshiEdgeDayMs);
}

/// How old the artifact on screen is. `stale` is true for every reason the age
/// cannot be vouched for — older than the threshold, dated in the future, or
/// undated — because each of those is a set of conclusions that must not
/// render as silently current.
struct KalshiEdgeReportFreshness {
    bool dated = false;
    qint64 age_ms = -1;   ///< -1 when there is no age to state
    bool stale = true;    ///< an undated report is stale, never current
    QString text;
};

inline KalshiEdgeReportFreshness kalshi_edge_report_freshness(
    const QJsonObject& report, qint64 now_ms, qint64 stale_after_ms = kKalshiEdgeReportStaleMs) {
    KalshiEdgeReportFreshness freshness;
    const QJsonValue ts = report.value(QStringLiteral("generated_at_ms"));
    if (!ts.isDouble()) {
        freshness.text = QStringLiteral("generated at an UNKNOWN time · the report carries no "
                                        "generated_at_ms, so its age cannot be stated");
        return freshness;
    }
    const auto generated_ms = static_cast<qint64>(ts.toDouble());
    if (generated_ms > now_ms) {
        // Mistrusted rather than clamped to zero, exactly as the status chip
        // mistrusts a future heartbeat.
        freshness.text = QStringLiteral("generated at a timestamp in the FUTURE, so its age is not "
                                        "trusted");
        return freshness;
    }
    freshness.dated = true;
    freshness.age_ms = now_ms - generated_ms;
    freshness.stale = freshness.age_ms > stale_after_ms;
    freshness.text = QStringLiteral("generated %1").arg(kalshi_edge_age_text(freshness.age_ms));
    if (freshness.stale)
        freshness.text += QStringLiteral(" · STALE (the job runs weekly; this report is older "
                                         "than %1 days)").arg(stale_after_ms / kKalshiEdgeDayMs);
    return freshness;
}

// --- reading the published file ---------------------------------------------

inline QString kalshi_edge_report_absent_reason() {
    return QStringLiteral("no %1 in the evidence directory — the edge-autopsy job has not "
                          "published lessons here yet (`kalshi bot lessons --refresh` runs it)")
        .arg(QString::fromLatin1(kKalshiEdgeReportFile));
}

/// The artifact as a reader found it. `available` is false for every reason a
/// reader could fail, and `why` says which — the renderers print that reason
/// and NO numbers.
struct KalshiEdgeReportFile {
    bool available = false;
    QString why = kalshi_edge_report_absent_reason();
    QJsonObject object;
};

inline KalshiEdgeReportFile kalshi_edge_read_report_file(const QString& path) {
    KalshiEdgeReportFile file;
    QFile handle(path);
    if (!handle.exists()) return file;
    if (!handle.open(QIODevice::ReadOnly | QIODevice::Text)) {
        file.why = QStringLiteral("%1 exists but cannot be opened").arg(path);
        return file;
    }
    const QJsonDocument document = QJsonDocument::fromJson(handle.readAll());
    if (!document.isObject()) {
        file.why = QStringLiteral("%1 is not parseable JSON — no lesson is read from it")
                       .arg(path);
        return file;
    }
    file.available = true;
    file.object = document.object();
    return file;
}

// --- the ONE formatter every surface renders --------------------------------

/// One rendered lesson: the whole line, plus the colour intent decided HERE
/// beside the verdict rather than re-derived by a widget from the text.
struct KalshiEdgeLesson {
    QString id;
    QString verdict;
    QString text;
    /// The same lesson with the claim and the key numbers dropped, for a
    /// surface too narrow to draw `text` without eliding it.
    ///
    /// This exists because of rule 1 and nothing else. The cockpit draws one
    /// line per lesson in a fixed-width scene; eliding `text` there cuts from
    /// the RIGHT, which is exactly where the sample size sits, so a narrow
    /// window silently produced the one rendering this card must never have —
    /// a conclusion with no denominator attached. `compact` front-loads the
    /// sample size so it survives any width, and a widget chooses between the
    /// two strings rather than composing a third.
    QString compact;
    QString role = QStringLiteral("grey");
};

/// The card, as the BOT tab, the cockpit and `kalshi bot lessons` all show it.
struct KalshiEdgeLessonsView {
    bool available = false;
    bool stale = false;
    QString header;       ///< the title line, carrying the artifact's age
    QString header_role = QStringLiteral("grey");
    QList<KalshiEdgeLesson> lessons;

    /// Header first, then one line per lesson — what the CLI prints and what
    /// the BOT tab's card joins with newlines. One list, so the two cannot
    /// diverge by a character.
    QStringList lines() const {
        QStringList out{header};
        for (const auto& lesson : lessons) out << lesson.text;
        return out;
    }
};

namespace kalshi_edge_lessons_detail {

/// The colour intent of one verdict. An unrecognised word is a warning, not a
/// default: a build that does not know a verdict must not paint it as normal.
inline QString verdict_role(const QString& verdict) {
    if (verdict == QLatin1String(kKalshiEdgeVerdictEdge)) return QStringLiteral("green");
    if (verdict == QLatin1String(kKalshiEdgeVerdictFeeEaten)) return QStringLiteral("amber");
    if (verdict == QLatin1String(kKalshiEdgeVerdictNoEdge)) return QStringLiteral("grey");
    if (verdict == QLatin1String(kKalshiEdgeVerdictInsufficient)) return QStringLiteral("grey");
    return QStringLiteral("amber");
}

/// Rule 1. The sample size, in the question's own unit — never blank, never a
/// zero standing in for an absence.
inline QString sample_text(const QJsonObject& lesson) {
    const QJsonObject sample = lesson.value(QStringLiteral("sample")).toObject();
    const QJsonValue n = sample.value(QStringLiteral("n"));
    const QString unit = sample.value(QStringLiteral("unit")).toString();
    if (!n.isDouble())
        return QStringLiteral("SAMPLE SIZE NOT STATED%1")
            .arg(unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit));
    QString text = QStringLiteral("n=%1 %2")
                       .arg(QString::number(n.toInt()),
                            unit.isEmpty() ? QStringLiteral("(unit not stated)") : unit);
    const QString detail = sample.value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty()) text += QStringLiteral(" (%1)").arg(detail);
    return text;
}

inline QString numbers_text(const QJsonObject& lesson) {
    QStringList parts;
    for (const auto& value : lesson.value(QStringLiteral("key_numbers")).toArray()) {
        const QJsonObject number = value.toObject();
        const QString label = number.value(QStringLiteral("label")).toString();
        // The text the driver formatted, verbatim. Re-formatting the raw value
        // here would be a second rounding for the same measurement.
        const QString text = number.value(QStringLiteral("text")).toString();
        if (label.isEmpty() || text.isEmpty()) continue;
        parts << QStringLiteral("%1 %2").arg(label, text);
    }
    return parts.join(QStringLiteral(" · "));
}

inline QString span_text(const QJsonObject& lesson) {
    const QJsonValue span = lesson.value(QStringLiteral("data_span"));
    if (!span.isObject()) return QStringLiteral("span NOT STATED");
    const QString text = span.toObject().value(QStringLiteral("text")).toString();
    return text.isEmpty() ? QStringLiteral("span NOT STATED") : text;
}

} // namespace kalshi_edge_lessons_detail

/// The lessons card. `file` is the artifact as a reader found it.
///
/// An unavailable file yields `available == false`, a header carrying the
/// refusal's reason, and NO lessons at all — the card shows exactly one line
/// and no digits.
inline KalshiEdgeLessonsView kalshi_edge_lessons(const KalshiEdgeReportFile& file, qint64 now_ms) {
    using namespace kalshi_edge_lessons_detail;
    KalshiEdgeLessonsView view;
    if (!file.available) {
        view.header = QStringLiteral("WHAT THE RECORD TEACHES · UNAVAILABLE · %1").arg(file.why);
        return view;
    }

    const KalshiEdgeReportFreshness freshness = kalshi_edge_report_freshness(file.object, now_ms);
    view.available = true;
    view.stale = freshness.stale;
    view.header_role = freshness.stale ? QStringLiteral("amber") : QStringLiteral("cyan");

    const QJsonArray lessons = file.object.value(QStringLiteral("lessons")).toArray();
    view.header = QStringLiteral("WHAT THE RECORD TEACHES · %1 lesson%2 from the edge autopsy · %3")
                      .arg(QString::number(lessons.size()),
                           lessons.size() == 1 ? QString() : QStringLiteral("s"), freshness.text);
    if (lessons.isEmpty()) {
        // A well-formed artifact that carries no lesson is not a card with
        // nothing wrong: it is a refresh that measured nothing, and it says so.
        view.header += QStringLiteral(" · the report carries NO lesson — nothing was measured");
        return view;
    }

    // A stale artifact carries its age on EVERY line, not only in the header:
    // these lines get read one at a time, and a conclusion scrolled past a
    // stale header must still read as stale (the funnel's rule, #153).
    const QString age_suffix =
        freshness.stale ? QStringLiteral(" · %1").arg(freshness.text) : QString();

    for (const auto& value : lessons) {
        const QJsonObject lesson = value.toObject();
        KalshiEdgeLesson out;
        out.id = lesson.value(QStringLiteral("id")).toString(QStringLiteral("?"));
        out.verdict =
            lesson.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
        out.role = verdict_role(out.verdict);
        // Rule 3: a stale report cannot paint anything green, whatever its
        // verdicts say.
        if (freshness.stale && out.role == QStringLiteral("green"))
            out.role = QStringLiteral("amber");

        const QString head = QStringLiteral("%1 %2").arg(
            out.id, lesson.value(QStringLiteral("title")).toString(QStringLiteral("(untitled)")));
        const QString sample = sample_text(lesson);   // rule 1: on every line, always
        const QString span = span_text(lesson);

        QStringList parts;
        parts << head << out.verdict;
        parts << lesson.value(QStringLiteral("claim")).toString(QStringLiteral("no claim stated"));
        const QString numbers = numbers_text(lesson);
        if (!numbers.isEmpty()) parts << numbers;
        parts << sample << span;
        out.text = parts.join(QStringLiteral(" · ")) + age_suffix;
        // Sample size third, ahead of everything a narrow surface would cut.
        out.compact = QStringList{head, out.verdict, sample, span}.join(QStringLiteral(" · ")) +
                      age_suffix;
        view.lessons << out;
    }
    return view;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
