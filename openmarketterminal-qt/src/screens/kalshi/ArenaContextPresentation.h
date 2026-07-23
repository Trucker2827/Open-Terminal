#pragma once

// Presentation for the Kalshi screen's ARENA context strip. Pure function of
// (arena-report.json contents, now) so the honesty rules are regression-
// testable without a widget: a missing report reads OFFLINE, an old report
// reads STALE, and the verdict is shown verbatim — INSUFFICIENT_DATA is
// never dressed up as anything else.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::screens::kalshi {

struct ArenaContextView {
    QString headline;     // verdict + rounds + freshness, or the OFFLINE/STALE state
    QString lane;         // top comparable lane, or the honest none-comparable reason
    bool offline = true;  // no readable arena-report.json
    bool stale = false;   // report exists but its freshness cannot be trusted
    bool leader = false;  // verdict declares a statistically separated leader
};

// The arena loop rewrites arena-report.json after every round attempt
// (arena_loop.py `run` regenerates it on a ~60s cadence plus round duration).
// Thirty minutes without a rewrite means the loop is not running.
inline constexpr qint64 kArenaReportStaleMs = 30LL * 60 * 1000;

inline QString arena_age_text(qint64 age_ms) {
    if (age_ms < 120'000) return QStringLiteral("%1s ago").arg(age_ms / 1'000);
    if (age_ms < 7'200'000) return QStringLiteral("%1m ago").arg(age_ms / 60'000);
    return QStringLiteral("%1h ago").arg(age_ms / 3'600'000);
}

inline ArenaContextView present_arena_context(const QJsonObject& report, qint64 now_ms) {
    ArenaContextView view;
    if (report.isEmpty()) {
        view.offline = true;
        view.headline =
            QStringLiteral("ARENA OFFLINE · no arena-report.json — is the arena loop running?");
        view.lane = QStringLiteral("Arena evidence unavailable; nothing to summarize.");
        return view;
    }
    view.offline = false;

    const QString verdict =
        report.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
    view.leader = verdict.startsWith(QStringLiteral("LEADER"));
    const int rounds = report.value(QStringLiteral("rounds_total")).toInt();

    const auto generated =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    const qint64 age_ms = now_ms - generated;
    view.stale = generated <= 0 || age_ms > kArenaReportStaleMs;
    const QString freshness = generated <= 0 ? QStringLiteral("age unknown")
        : age_ms < 0                         ? QStringLiteral("clock skew")
                                             : arena_age_text(age_ms);
    view.headline = QStringLiteral("%1%2 · %3 rounds · report %4")
        .arg(view.stale ? QStringLiteral("ARENA STALE · ") : QStringLiteral("ARENA "),
             verdict)
        .arg(rounds)
        .arg(freshness);

    // Top comparable lane by rank; when none qualifies, surface the honest
    // per-lane reason of the closest contender instead of inventing a leader.
    const QJsonArray leaderboard = report.value(QStringLiteral("leaderboard")).toArray();
    QJsonObject top;
    bool have_top = false;
    QJsonObject closest;
    double closest_coverage = -1.0;
    for (const auto& value : leaderboard) {
        const QJsonObject lane = value.toObject();
        if (lane.value(QStringLiteral("comparable")).toBool()) {
            if (!have_top || lane.value(QStringLiteral("rank")).toInt(1'000'000) <
                                 top.value(QStringLiteral("rank")).toInt(1'000'000)) {
                top = lane;
                have_top = true;
            }
        } else if (lane.value(QStringLiteral("coverage")).toDouble(-1.0) > closest_coverage) {
            closest_coverage = lane.value(QStringLiteral("coverage")).toDouble(-1.0);
            closest = lane;
        }
    }
    if (have_top) {
        const QJsonValue brier = top.value(QStringLiteral("brier"));
        const QJsonValue coverage = top.value(QStringLiteral("coverage"));
        view.lane = QStringLiteral("TOP LANE %1 · BRIER %2 · COVERAGE %3")
            .arg(top.value(QStringLiteral("id")).toString(QStringLiteral("--")),
                 brier.isDouble() ? QString::number(brier.toDouble(), 'f', 4)
                                  : QStringLiteral("--"),
                 coverage.isDouble()
                     ? QStringLiteral("%1%").arg(coverage.toDouble() * 100.0, 0, 'f', 0)
                     : QStringLiteral("--"));
    } else if (leaderboard.isEmpty()) {
        view.lane = QStringLiteral("NO LANES REPORTED YET");
    } else {
        view.lane = QStringLiteral("NO COMPARABLE LANE · best %1: %2")
            .arg(closest.value(QStringLiteral("id")).toString(QStringLiteral("--")),
                 closest.value(QStringLiteral("reason"))
                     .toString(QStringLiteral("not comparable")));
    }
    return view;
}

} // namespace openmarketterminal::screens::kalshi
