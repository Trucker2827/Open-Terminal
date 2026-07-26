#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace openmarketterminal::screens::kalshi {

struct AdvisorCanaryView {
    QString legacy_badge;
    QString canary_badge;
    QString system;
    QString qualification;
    QString safety;
    QString activity;
    bool legacy_live = false;
    bool canary_live = false;
    bool critical = true;
};

inline qint64 advisor_i64(const QJsonValue& value) {
    return value.isString() ? value.toString().toLongLong()
                            : static_cast<qint64>(value.toDouble());
}

inline QString advisor_join_blockers(const QJsonArray& values) {
    QStringList out;
    for (const auto& value : values) out << value.toString();
    return out.isEmpty() ? QStringLiteral("none") : out.join(QStringLiteral(", "));
}

inline AdvisorCanaryView present_advisor_canary(const QJsonObject& loop,
                                                 const QJsonObject& qualification_snapshot,
                                                 const QJsonObject& promotion,
                                                 const QJsonObject& safety,
                                                 const QJsonObject& canary,
                                                 const QJsonObject& latest,
                                                 const QJsonObject& legacy,
                                                 qint64 now_ms) {
    AdvisorCanaryView view;
    view.legacy_live = legacy.value(QStringLiteral("session_active")).toBool();
    const bool legacy_known = !legacy.isEmpty();
    view.legacy_badge = legacy_known
        ? QStringLiteral("LEGACY LIVE SESSION: %1").arg(view.legacy_live ? QStringLiteral("ARMED")
                                                                        : QStringLiteral("DISARMED"))
        : QStringLiteral("LEGACY LIVE SESSION: UNKNOWN / FAIL CLOSED");

    const QString promotion_state = promotion.value(QStringLiteral("state")).toString();
    view.canary_live = canary.value(QStringLiteral("enabled")).toBool() &&
                       promotion_state == QStringLiteral("CANARY_ENABLED");
    view.canary_badge = QStringLiteral("CODEX CANARY: %1")
        .arg(promotion_state.isEmpty() ? QStringLiteral("UNKNOWN / FAIL CLOSED")
                                      : view.canary_live ? QStringLiteral("ENABLED") : promotion_state);

    const qint64 heartbeat = advisor_i64(loop.value(QStringLiteral("heartbeat_at_ms")));
    const bool heartbeat_fresh = heartbeat > 0 && heartbeat <= now_ms && now_ms - heartbeat <= 180'000;
    const bool journal_valid = loop.value(QStringLiteral("journal_valid")).toBool(false);
    const QString loop_version = loop.value(QStringLiteral("loop_version")).toString(QStringLiteral("unknown"));
    const QJsonObject score = qualification_snapshot.value(QStringLiteral("score")).toObject();
    const QJsonObject filter = score.value(QStringLiteral("filter")).toObject();
    const QString epoch = filter.value(QStringLiteral("forecaster_id")).toString(QStringLiteral("unknown"));
    view.system = QStringLiteral("SUPERVISOR %1 · %2 · PID %3\nFORECASTER %4\nCAPABILITY LOCK %5 · JOURNAL %6 · %7 opportunities")
        .arg(heartbeat_fresh ? QStringLiteral("LIVE") : QStringLiteral("STALE / UNKNOWN"), loop_version)
        .arg(loop.value(QStringLiteral("pid")).toInt())
        .arg(epoch)
        .arg(epoch.contains(QStringLiteral("zero-capability")) ? QStringLiteral("PINNED")
                                                               : QStringLiteral("UNKNOWN / FAIL CLOSED"))
        .arg(journal_valid ? QStringLiteral("VALID") : QStringLiteral("INVALID / UNKNOWN"))
        .arg(loop.value(QStringLiteral("opportunities")).toInt());

    const QJsonObject qual = qualification_snapshot.value(QStringLiteral("qualification")).toObject();
    const QJsonObject metrics = qual.value(QStringLiteral("metrics")).toObject();
    const QJsonObject checks = qual.value(QStringLiteral("checks")).toObject();
    const QJsonObject policy = qual.value(QStringLiteral("policy")).toObject();
    QStringList check_text;
    const QStringList check_names{QStringLiteral("minimum_resolved"), QStringLiteral("daemon_coverage"),
        QStringLiteral("positive_daemon_improvement"), QStringLiteral("positive_market_improvement"),
        QStringLiteral("confidence_interval"), QStringLiteral("net_value_after_fees")};
    for (const QString& name : check_names)
        check_text << QStringLiteral("%1 %2").arg(checks.value(name).toBool() ? QStringLiteral("✓")
                                                                            : QStringLiteral("✗"), name);
    view.qualification = QStringLiteral("QUALIFICATION %1 · %2 / %3 resolved · daemon coverage %4%\n%5\nQualification never enables trading by itself.")
        .arg(qual.value(QStringLiteral("qualified")).toBool() ? QStringLiteral("PASSED") : QStringLiteral("NOT PASSED"))
        .arg(metrics.value(QStringLiteral("resolved")).toInt())
        .arg(policy.value(QStringLiteral("minimum_resolved")).toInt(200))
        .arg(metrics.value(QStringLiteral("daemon_coverage")).toDouble() * 100.0, 0, 'f', 1)
        .arg(check_text.join(QStringLiteral(" · ")));

    const QJsonArray blockers = safety.value(QStringLiteral("blockers")).toArray();
    const QString scope = safety.value(QStringLiteral("drawdown_scope")).toString(QStringLiteral("unknown"));
    view.safety = QStringLiteral("PROMOTION %1 · SAFETY %2\nBLOCKERS: %3\nP&L pending %4 · reconciliation age %5 · daily P&L $%6 · epoch drawdown $%7 · losses %8 · exposure $%9\nCANARY enabled=%10 · epoch=%11 · limits $%12/order, $%13 exposure, $%14 daily loss")
        .arg(promotion_state.isEmpty() ? QStringLiteral("UNKNOWN") : promotion_state,
             safety.value(QStringLiteral("safe")).toBool() ? QStringLiteral("CLEAR") : QStringLiteral("BLOCKED"),
             advisor_join_blockers(blockers))
        .arg(safety.value(QStringLiteral("pnl_reconciliation_pending")).toInt())
        .arg(advisor_i64(safety.value(QStringLiteral("last_reconciled_at_ms"))) > 0
                 ? QStringLiteral("%1s").arg(qMax<qint64>(0, now_ms-advisor_i64(safety.value(QStringLiteral("last_reconciled_at_ms"))))/1000)
                 : QStringLiteral("unknown"))
        .arg(safety.value(QStringLiteral("daily_realized_pnl")).toDouble(),0,'f',2)
        .arg(safety.value(QStringLiteral("maximum_drawdown")).toDouble(),0,'f',2)
        .arg(safety.value(QStringLiteral("consecutive_losses")).toInt())
        .arg(safety.value(QStringLiteral("open_exposure")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("enabled")).toBool() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(scope == QStringLiteral("canary_epoch") ? QString::number(advisor_i64(canary.value(QStringLiteral("epoch_started_at_ms"))))
                                                     : QStringLiteral("unknown"))
        .arg(canary.value(QStringLiteral("max_order_dollars")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("max_open_exposure")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("daily_loss_limit")).toDouble(),0,'f',2);

    const QJsonObject forecast = latest.value(QStringLiteral("forecast")).toObject();
    const QJsonObject proposal = latest.value(QStringLiteral("proposal")).toObject();
    QString reason = latest.value(QStringLiteral("reason_code")).toString();
    if (reason.isEmpty()) reason = forecast.value(QStringLiteral("reason_code")).toString();
    view.activity = QStringLiteral("LATEST %1 · %2%3\nPROPOSAL %4 %5 @ %6 · NET EDGE %7 · GATE %8")
        .arg(latest.value(QStringLiteral("status")).toString(QStringLiteral("UNKNOWN")),
             latest.value(QStringLiteral("ticker")).toString(QStringLiteral("—")),
             reason.isEmpty() ? QString() : QStringLiteral(" · %1").arg(reason),
             proposal.value(QStringLiteral("side")).toString(QStringLiteral("—")).toUpper(),
             QString::number(proposal.value(QStringLiteral("contracts")).toInt()),
             QString::number(proposal.value(QStringLiteral("limit_price")).toDouble(), 'f', 2),
             QString::number(proposal.value(QStringLiteral("cost_net_edge")).toDouble(), 'f', 4),
             proposal.value(QStringLiteral("gate")).toString(QStringLiteral("—")).toUpper());

    view.critical = !heartbeat_fresh || !journal_valid || promotion_state.isEmpty() ||
                    !safety.value(QStringLiteral("safe")).toBool() || view.canary_live;
    return view;
}

// ---------------------------------------------------------------------------
// Retirement of the blind-duel advisor protocol.
//
// The advisor loop rewrote every one of its state files on each pass, so when
// all of them fall silent together the loop is not slow — it has stopped, and
// the tab must say so instead of rendering day-old duel machinery as if it
// were live. This is a fact about the advisor_* files ONLY: the legacy live
// session status is polled from the running terminal and deliberately never
// enters this decision, so a concluded duel can never mask an armed session.
// ---------------------------------------------------------------------------

// The loop's own heartbeat bound is 180s. Six hours without a single write
// from ANY advisor file is far past a hiccup, a restart, or a slow forecast;
// nothing shorter than that is claimed to be retirement.
inline constexpr qint64 kAdvisorRetiredAfterMs = 6LL * 60 * 60 * 1000;

struct AdvisorDuelRetirement {
    bool retired = false;       // every advisor file is silent past the threshold
    qint64 last_update_ms = 0;  // newest advisor write seen, 0 when nothing is known
    bool skewed = false;        // an advisor file is dated in the future: unreadable
};

inline AdvisorDuelRetirement advisor_duel_retirement(const QJsonObject& loop,
                                                     const QJsonObject& qualification_snapshot,
                                                     const QJsonObject& promotion,
                                                     const QJsonObject& safety,
                                                     const QJsonObject& canary,
                                                     const QJsonObject& latest,
                                                     qint64 now_ms) {
    AdvisorDuelRetirement out;
    // Only write timestamps count. Fields such as canary_epoch_started_at_ms
    // or last_reconciled_at_ms describe the epoch, not when the file was last
    // written, and would date the loop by something it did not do.
    bool skewed = false;
    const auto note = [&](const QJsonValue& value) {
        const qint64 ms = advisor_i64(value);
        if (ms <= 0) return;
        if (ms > now_ms) { skewed = true; return; }
        out.last_update_ms = qMax(out.last_update_ms, ms);
    };
    note(loop.value(QStringLiteral("heartbeat_at_ms")));
    note(loop.value(QStringLiteral("updated_at_ms")));
    note(qualification_snapshot.value(QStringLiteral("updated_at_ms")));
    note(promotion.value(QStringLiteral("updated_at_ms")));
    note(safety.value(QStringLiteral("updated_at_ms")));
    note(canary.value(QStringLiteral("updated_at_ms")));
    note(latest.value(QStringLiteral("opened_at_ms")));
    // Fail closed twice over: absent files are not evidence of retirement, and
    // a future-dated write is unreadable rather than old. Both keep the
    // existing UNKNOWN / FAIL CLOSED rendering rather than declaring an end.
    out.skewed = skewed;
    out.retired = !skewed && out.last_update_ms > 0 &&
                  now_ms - out.last_update_ms > kAdvisorRetiredAfterMs;
    return out;
}

struct AdvisorDuelArchive {
    QString headline;              // RETIRED / DUEL CONCLUDED + the last advisor write
    QString record;                // the frozen final record, or why it is unavailable
    bool record_available = false; // a readable advisor_competition_report.json
};

// scoring_infrastructure_hash covers a fixed manifest, and the report JSON does
// not carry its length — so the count is quoted from the manifest itself:
// competition_report.py's SCORING_INFRASTRUCTURE tuple, seven runtime Python
// components (advisor_core, advisor_loop, blind_prompt, claude_cli_forecaster,
// codex_forecaster, competition_report, ../prediction_kalshi). Those are the
// same seven files the charter freezes, so the number cannot drift without a
// charter exception. It is a reference to the manifest, never a figure derived
// from the report.
inline constexpr int kAdvisorScoringManifestFiles = 7;

inline QString advisor_retired_age_text(qint64 age_ms) {
    if (age_ms < 7'200'000) return QStringLiteral("%1m").arg(age_ms / 60'000);
    if (age_ms < 172'800'000) return QStringLiteral("%1h").arg(age_ms / 3'600'000);
    return QStringLiteral("%1d").arg(age_ms / 86'400'000);
}

// The final record is read out of advisor_competition_report.json — the report
// competition_report.py froze when the duel ended. Nothing here is derived:
// result_state is printed as the report states it, and a report that declares
// no winner is never talked into one.
inline AdvisorDuelArchive present_advisor_duel_archive(const QJsonObject& competition_report,
                                                       qint64 last_update_ms, qint64 now_ms) {
    AdvisorDuelArchive archive;
    const QString when = last_update_ms > 0
        ? QDateTime::fromMSecsSinceEpoch(last_update_ms, QTimeZone::UTC)
              .toString(QStringLiteral("yyyy-MM-dd hh:mm 'UTC'"))
        : QStringLiteral("unknown");
    const QString age = last_update_ms > 0 && now_ms >= last_update_ms
        ? QStringLiteral(" (%1 ago)").arg(advisor_retired_age_text(now_ms - last_update_ms))
        : QString();
    archive.headline =
        QStringLiteral("DUEL CONCLUDED · ADVISOR LOOP RETIRED — last advisor write %1%2\n"
                       "No advisor state has changed since. Nothing in this card is live; "
                       "the current Kalshi automation path is the BOT tab.")
            .arg(when, age);

    if (competition_report.isEmpty()) {
        archive.record = QStringLiteral(
            "FINAL FROZEN DUEL RECORD UNAVAILABLE — advisor_competition_report.json is missing "
            "or unreadable. The duel's record cannot be shown, and nothing is inferred from its "
            "absence.");
        return archive;
    }
    archive.record_available = true;

    const QJsonObject epochs = competition_report.value(QStringLiteral("epoch_pair")).toObject();
    const QString claude_epoch =
        epochs.value(QStringLiteral("claude")).toString(QStringLiteral("unknown"));
    const QString codex_epoch =
        epochs.value(QStringLiteral("codex")).toString(QStringLiteral("unknown"));
    const QString state = competition_report.value(QStringLiteral("result_state")).toString();
    const QString verdict =
        state == QStringLiteral("CLAUDE_WINS") ? QStringLiteral("WINNER: CLAUDE — %1").arg(claude_epoch)
        : state == QStringLiteral("CODEX_WINS") ? QStringLiteral("WINNER: CODEX — %1").arg(codex_epoch)
        : state.isEmpty() ? QStringLiteral("NO WINNER DECLARED — result_state missing")
                          : QStringLiteral("NO WINNER DECLARED — %1").arg(state);

    const QJsonObject thresholds = competition_report.value(QStringLiteral("thresholds")).toObject();
    const QJsonObject coverage = competition_report.value(QStringLiteral("coverage")).toObject();
    const QString hash =
        competition_report.value(QStringLiteral("scoring_infrastructure_hash")).toString();
    archive.record =
        QStringLiteral("FINAL FROZEN DUEL RECORD (advisor_competition_report.json)\n"
                       "RESULT %1 · %2 / %3 jointly resolved of %4 opportunities\n"
                       "EPOCHS claude=%5 · codex=%6\n"
                       "COVERAGE claude %7% · codex %8% (minimum %9% each)\n"
                       "SCORING FROZEN · infrastructure %10 (SHA-256 over the %13-file "
                       "SCORING_INFRASTRUCTURE manifest) · shadow_only=%11 · execution_eligible=%12")
            .arg(verdict)
            .arg(competition_report.value(QStringLiteral("jointly_resolved")).toInt())
            .arg(thresholds.value(QStringLiteral("minimum_jointly_resolved")).toInt())
            .arg(competition_report.value(QStringLiteral("opportunities")).toInt())
            .arg(claude_epoch, codex_epoch)
            .arg(coverage.value(QStringLiteral("claude")).toDouble() * 100.0, 0, 'f', 1)
            .arg(coverage.value(QStringLiteral("codex")).toDouble() * 100.0, 0, 'f', 1)
            .arg(thresholds.value(QStringLiteral("minimum_coverage_each")).toDouble() * 100.0, 0, 'f', 1)
            .arg(hash.isEmpty() ? QStringLiteral("unknown") : hash.left(12))
            .arg(competition_report.value(QStringLiteral("shadow_only")).toBool()
                     ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(competition_report.value(QStringLiteral("execution_eligible")).toBool()
                     ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(kAdvisorScoringManifestFiles);
    return archive;
}

// ---------------------------------------------------------------------------
// The concluded duel as an Alpha Arena card (issue #151).
//
// The ADVISOR & CANARY tab is gone from the Kalshi tab row — a permanently
// dead control among living ones. What survives is the record, rendered
// beside the arena context from the same archived advisor evidence the
// retired tab read.
//
// Three states, and the third is why `retired` alone cannot drive this card.
// advisor_duel_retirement fails closed twice over: absent files and a
// future-dated write both come back `retired == false`. So "not retired"
// spans BOTH "the loop is writing again" and "there is nothing to read", and
// reading the second as a resurrection would put the dead tab back on a
// machine that never ran the duel. LIVE AGAIN is claimed only on positive
// evidence of a recent advisor write — the same fail-closed reflex, pointed
// in the other direction.
// ---------------------------------------------------------------------------

enum class AdvisorDuelPresence {
    Archived,    // every advisor file silent past kAdvisorRetiredAfterMs
    LiveAgain,   // an advisor file was written recently: the loop restarted
    NoEvidence,  // nothing readable, or a future-dated write: neither is claimed
};

inline AdvisorDuelPresence advisor_duel_presence(const AdvisorDuelRetirement& retirement) {
    if (retirement.retired) return AdvisorDuelPresence::Archived;
    if (retirement.skewed || retirement.last_update_ms <= 0)
        return AdvisorDuelPresence::NoEvidence;
    return AdvisorDuelPresence::LiveAgain;
}

struct ConcludedDuelCard {
    AdvisorDuelPresence presence = AdvisorDuelPresence::NoEvidence;
    QString state;                 // ARCHIVED / LIVE AGAIN / NO ADVISOR EVIDENCE
    QString record;                // the frozen final record, or why it is unavailable
    bool record_available = false; // a readable advisor_competition_report.json
    // The resurrection guard: on LIVE AGAIN the old ADVISOR & CANARY panel
    // comes back into the tab row, so a restarted loop is never invisible.
    bool advisor_panel_reachable = false;
};

inline ConcludedDuelCard present_concluded_duel_card(const QJsonObject& competition_report,
                                                     const AdvisorDuelRetirement& retirement,
                                                     qint64 now_ms) {
    ConcludedDuelCard card;
    card.presence = advisor_duel_presence(retirement);
    // The frozen record is on disk whatever the loop is doing, so it is never
    // gated on the verdict: dropping it in the states that are not ARCHIVED
    // would lose data the retired tab used to show.
    const AdvisorDuelArchive archive =
        present_advisor_duel_archive(competition_report, retirement.last_update_ms, now_ms);
    card.record = archive.record;
    card.record_available = archive.record_available;

    switch (card.presence) {
        case AdvisorDuelPresence::Archived:
            card.state = QStringLiteral("CONCLUDED DUEL (v5) · ARCHIVED\n") + archive.headline;
            break;
        case AdvisorDuelPresence::LiveAgain: {
            card.advisor_panel_reachable = true;
            const qint64 age = qMax<qint64>(0, now_ms - retirement.last_update_ms);
            card.state = QStringLiteral(
                "CONCLUDED DUEL (v5) · LIVE AGAIN — an advisor file was written %1 ago. "
                "The advisor loop is running, so this is no longer an archive: the "
                "ADVISOR & CANARY panel is back in the tab row with the live readout.")
                             .arg(advisor_retired_age_text(age));
            break;
        }
        case AdvisorDuelPresence::NoEvidence:
            card.state = retirement.skewed
                ? QStringLiteral(
                      "CONCLUDED DUEL (v5) · NO READABLE ADVISOR STATE — an advisor file is "
                      "dated in the future (clock skew). Neither retirement nor a running "
                      "loop is claimed.")
                : QStringLiteral(
                      "CONCLUDED DUEL (v5) · NO ADVISOR EVIDENCE — no advisor state files on "
                      "this machine. Neither retirement nor a running loop is claimed.");
            break;
    }
    return card;
}

} // namespace openmarketterminal::screens::kalshi
