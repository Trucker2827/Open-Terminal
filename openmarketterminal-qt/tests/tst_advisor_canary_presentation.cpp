#include <QtTest>

#include "screens/kalshi/AdvisorCanaryPresentation.h"

using namespace openmarketterminal::screens::kalshi;

// A loop state file written at `at` — the shape the advisor loop left behind.
static QJsonObject stale_loop(qint64 at) {
    return QJsonObject{{"heartbeat_at_ms", double(at)}, {"updated_at_ms", double(at)},
        {"journal_valid", true}, {"loop_version", "kalshi-advisor-loop-v1"}, {"pid", 32396},
        {"opportunities", 2256}};
}

class AdvisorCanaryPresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void missing_state_is_unknown_and_fail_closed() {
        const auto view = present_advisor_canary({}, {}, {}, {}, {}, {}, {}, 1'000'000);
        QVERIFY(view.critical);
        QVERIFY(view.legacy_badge.contains("UNKNOWN / FAIL CLOSED"));
        QVERIFY(view.canary_badge.contains("UNKNOWN / FAIL CLOSED"));
        QVERIFY(view.system.contains("STALE / UNKNOWN"));
        QVERIFY(view.system.contains("INVALID / UNKNOWN"));
    }

    void legacy_armed_is_distinct_from_disabled_canary() {
        const qint64 now = 2'000'000;
        const QJsonObject loop{{"heartbeat_at_ms",double(now-1'000)}, {"journal_valid",true},
            {"loop_version","kalshi-advisor-loop-v1"}, {"pid",42}, {"opportunities",17}};
        const QJsonObject qualification{{"score",QJsonObject{{"filter",QJsonObject{
            {"forecaster_id","codex-unattended/kalshi-blind-codex-v3-zero-capability"}}}}},
            {"qualification",QJsonObject{{"qualified",false},{"metrics",QJsonObject{{"resolved",0}}},
                {"policy",QJsonObject{{"minimum_resolved",200}}},{"checks",QJsonObject{}}}}};
        const QJsonObject promotion{{"state","PAUSED"}};
        const QJsonObject safety{{"safe",false},{"blockers",QJsonArray{"reconciliation_stale"}},
            {"drawdown_scope","canary_epoch"}};
        const QJsonObject canary{{"enabled",false},{"epoch_started_at_ms",1.0},
            {"max_order_dollars",2.0},{"max_open_exposure",5.0},{"daily_loss_limit",5.0}};
        const auto view = present_advisor_canary(loop,qualification,promotion,safety,canary,{},
            QJsonObject{{"session_active",true}},now);
        QVERIFY(view.legacy_live);
        QVERIFY(!view.canary_live);
        QCOMPARE(view.legacy_badge,QStringLiteral("LEGACY LIVE SESSION: ARMED"));
        QCOMPARE(view.canary_badge,QStringLiteral("CODEX CANARY: PAUSED"));
        QVERIFY(view.safety.contains("reconciliation_stale"));
    }

    void qualification_and_abstention_are_visible() {
        const QJsonObject qualification{{"score",QJsonObject{{"filter",QJsonObject{
            {"forecaster_id","codex-unattended/kalshi-blind-codex-v3-zero-capability"}}}}},
            {"qualification",QJsonObject{{"qualified",false},
                {"metrics",QJsonObject{{"resolved",73},{"daemon_coverage",0.91}}},
                {"policy",QJsonObject{{"minimum_resolved",200}}},
                {"checks",QJsonObject{{"daemon_coverage",true}}}}}};
        const QJsonObject latest{{"status","ABSTAINED"},{"ticker","KXBTC"},
            {"reason_code","CAPABILITY_LOCKDOWN_FAILED"}};
        const auto view = present_advisor_canary({},qualification,QJsonObject{{"state","SHADOW"}},
            QJsonObject{{"safe",false}},QJsonObject{{"enabled",false}},latest,{},1'000);
        QVERIFY(view.qualification.contains("73 / 200"));
        QVERIFY(view.qualification.contains("91.0%"));
        QVERIFY(view.activity.contains("CAPABILITY_LOCKDOWN_FAILED"));
    }

    // ---- retirement of the duel protocol (issue #138) ----------------------
    //
    // The six advisor_* files on this machine stopped moving on 2026-07-23 and
    // no advisor job exists any more. The tab must say the duel concluded
    // instead of rendering day-old machinery as a live system — and must go
    // straight back to the live rendering if the loop ever restarts.

    void every_advisor_file_silent_is_retired() {
        const qint64 now = 1'800'000'000'000;
        const qint64 wrote = now - 28LL * 3'600'000;  // ~28h, this machine's real gap
        const auto out = advisor_duel_retirement(stale_loop(wrote),
            QJsonObject{{"updated_at_ms",double(wrote)}}, QJsonObject{{"updated_at_ms",double(wrote-1'000)}},
            QJsonObject{{"updated_at_ms",double(wrote-2'000)}}, QJsonObject{{"updated_at_ms",double(wrote)}},
            QJsonObject{{"opened_at_ms",double(wrote-3'000)}}, now);
        QVERIFY(out.retired);
        QCOMPARE(out.last_update_ms, wrote);
    }

    // Criterion: a genuinely running loop must never be masked. Each advisor
    // write timestamp on its own is enough to keep the live rendering.
    void any_fresh_advisor_file_is_not_retired() {
        const qint64 now = 1'800'000'000'000;
        const qint64 wrote = now - 28LL * 3'600'000;
        const qint64 fresh = now - 30'000;
        const QStringList fields{"loop.heartbeat_at_ms", "loop.updated_at_ms",
            "qualification.updated_at_ms", "promotion.updated_at_ms", "safety.updated_at_ms",
            "canary.updated_at_ms", "latest.opened_at_ms"};
        for (int i = 0; i < fields.size(); ++i) {
            QJsonObject loop = stale_loop(wrote);
            QJsonObject qualification{{"updated_at_ms",double(wrote)}};
            QJsonObject promotion{{"updated_at_ms",double(wrote)}};
            QJsonObject safety{{"updated_at_ms",double(wrote)}};
            QJsonObject canary{{"updated_at_ms",double(wrote)}};
            QJsonObject latest{{"opened_at_ms",double(wrote)}};
            switch (i) {
                case 0: loop["heartbeat_at_ms"] = double(fresh); break;
                case 1: loop["updated_at_ms"] = double(fresh); break;
                case 2: qualification["updated_at_ms"] = double(fresh); break;
                case 3: promotion["updated_at_ms"] = double(fresh); break;
                case 4: safety["updated_at_ms"] = double(fresh); break;
                case 5: canary["updated_at_ms"] = double(fresh); break;
                default: latest["opened_at_ms"] = double(fresh); break;
            }
            const auto out = advisor_duel_retirement(loop,qualification,promotion,safety,canary,latest,now);
            QVERIFY2(!out.retired, qPrintable(QStringLiteral("fresh %1 was treated as retired")
                                                  .arg(fields.at(i))));
            QCOMPARE(out.last_update_ms, fresh);
        }
    }

    void retirement_threshold_is_exact() {
        const qint64 now = 1'800'000'000'000;
        const QJsonObject at_bound{{"updated_at_ms",double(now-kAdvisorRetiredAfterMs)}};
        const QJsonObject one_past{{"updated_at_ms",double(now-kAdvisorRetiredAfterMs-1)}};
        QVERIFY(!advisor_duel_retirement({},{},at_bound,{},{},{},now).retired);
        QVERIFY(advisor_duel_retirement({},{},one_past,{},{},{},now).retired);
    }

    // Absent files are not evidence that the duel ended, and a future-dated
    // write is unreadable rather than old: both stay UNKNOWN / FAIL CLOSED.
    void missing_or_skewed_state_is_not_retired() {
        const qint64 now = 1'800'000'000'000;
        const auto missing = advisor_duel_retirement({},{},{},{},{},{},now);
        QVERIFY(!missing.retired);
        QCOMPARE(missing.last_update_ms, qint64(0));
        const auto skewed = advisor_duel_retirement(
            stale_loop(now - 28LL*3'600'000), {}, {}, {},
            QJsonObject{{"updated_at_ms",double(now+60'000)}}, {}, now);
        QVERIFY(!skewed.retired);
    }

    // The legacy live session is a live poll of the running terminal, not an
    // advisor file: a concluded duel must never hide an armed session.
    void retirement_never_masks_an_armed_legacy_session() {
        const qint64 now = 1'800'000'000'000;
        const qint64 wrote = now - 28LL * 3'600'000;
        const QJsonObject armed{{"session_active",true}};
        const auto out = advisor_duel_retirement(stale_loop(wrote),{},{},{},{},{},now);
        QVERIFY(out.retired);
        const auto view = present_advisor_canary(stale_loop(wrote),{},{},{},{},{},armed,now);
        QVERIFY(view.legacy_live);
        QCOMPARE(view.legacy_badge, QStringLiteral("LEGACY LIVE SESSION: ARMED"));
    }

    void archive_headline_carries_the_last_advisor_write() {
        const qint64 wrote = 1'784'816'685'044;  // 2026-07-23 14:24 UTC
        const auto archive = present_advisor_duel_archive({}, wrote, wrote + 28LL*3'600'000);
        QVERIFY(archive.headline.contains("DUEL CONCLUDED"));
        QVERIFY(archive.headline.contains("2026-07-23 14:24 UTC"));
        QVERIFY(archive.headline.contains("(28h ago)"));
        QVERIFY(archive.headline.contains("BOT tab"));
    }

    // A report that declares no winner is never talked into one.
    void archive_reports_the_frozen_verdict_verbatim() {
        const QJsonObject report{{"result_state","INSUFFICIENT_PAIRED_DATA"},
            {"jointly_resolved",53}, {"opportunities",264},
            {"epoch_pair",QJsonObject{{"claude","kalshi-blind-claude-cli-v5-latency-neutral"},
                {"codex","kalshi-blind-codex-v4-zero-capability-latency-neutral"}}},
            {"coverage",QJsonObject{{"claude",0.25},{"codex",0.9848484848484849}}},
            {"thresholds",QJsonObject{{"minimum_jointly_resolved",200},{"minimum_coverage_each",0.8}}},
            {"scoring_infrastructure_hash","2876e683b880cdc77c82a5dcc1e32662cd54188668434d51cdccb19d6c3b7a48"},
            {"shadow_only",true},{"execution_eligible",false}};
        const auto archive = present_advisor_duel_archive(report, 1'784'816'685'044, 1'800'000'000'000);
        QVERIFY(archive.record_available);
        QVERIFY(archive.record.contains("NO WINNER DECLARED — INSUFFICIENT_PAIRED_DATA"));
        QVERIFY(!archive.record.contains("WINNER: "));
        QVERIFY(archive.record.contains("53 / 200 jointly resolved of 264 opportunities"));
        QVERIFY(archive.record.contains("claude=kalshi-blind-claude-cli-v5-latency-neutral"));
        QVERIFY(archive.record.contains("codex=kalshi-blind-codex-v4-zero-capability-latency-neutral"));
        QVERIFY(archive.record.contains("claude 25.0% · codex 98.5% (minimum 80.0% each)"));
        QVERIFY(archive.record.contains("SCORING FROZEN · infrastructure 2876e683b880"));
        QVERIFY(archive.record.contains("shadow_only=true · execution_eligible=false"));
    }

    void archive_names_a_winner_only_when_the_report_does() {
        const QJsonObject epochs{{"epoch_pair",QJsonObject{{"claude","claude-v5"},{"codex","codex-v4"}}}};
        auto record_for = [&epochs](const QString& state) {
            QJsonObject report = epochs;
            report.insert(QStringLiteral("result_state"), state);
            return present_advisor_duel_archive(report, 1'000, 2'000).record;
        };
        QVERIFY(record_for("CODEX_WINS").contains("WINNER: CODEX — codex-v4"));
        QVERIFY(record_for("CLAUDE_WINS").contains("WINNER: CLAUDE — claude-v5"));
        QVERIFY(record_for("STATISTICAL_TIE").contains("NO WINNER DECLARED — STATISTICAL_TIE"));
        QVERIFY(record_for("INVALID_EPOCH").contains("NO WINNER DECLARED — INVALID_EPOCH"));
        QVERIFY(record_for("").contains("NO WINNER DECLARED — result_state missing"));
    }

    // Missing data reads missing: no zero-filled record, no invented verdict.
    void missing_duel_report_reads_missing() {
        const auto archive = present_advisor_duel_archive({}, 1'784'816'685'044, 1'800'000'000'000);
        QVERIFY(!archive.record_available);
        QVERIFY(archive.record.contains("FINAL FROZEN DUEL RECORD UNAVAILABLE"));
        QVERIFY(archive.record.contains("advisor_competition_report.json"));
        QVERIFY(!archive.record.contains("WINNER"));
        QVERIFY(!archive.record.contains("0 / 0"));
    }

    // ---- the record as an Alpha Arena card (issue #151) --------------------
    //
    // The ADVISOR & CANARY tab is off the Kalshi tab row; the duel's frozen
    // record renders beside the arena context instead. These are the
    // both-directions retirement tests above, re-pointed at the surface the
    // behaviour moved to: a dead loop must not put the tab back, and a loop
    // that starts writing again must.

    void an_archived_duel_reads_archived_and_keeps_the_tab_off_the_row() {
        const qint64 now = 1'800'000'000'000;
        const qint64 wrote = now - 28LL * 3'600'000;
        const auto retirement = advisor_duel_retirement(stale_loop(wrote), {}, {}, {}, {}, {}, now);
        QVERIFY(retirement.retired);
        const auto card = present_concluded_duel_card(report_v5(), retirement, now);
        QCOMPARE(card.presence, AdvisorDuelPresence::Archived);
        QVERIFY(!card.advisor_panel_reachable);
        QVERIFY(card.state.contains("CONCLUDED DUEL (v5) · ARCHIVED"));
        QVERIFY(card.state.contains("DUEL CONCLUDED"));
        QVERIFY(!card.state.contains("LIVE AGAIN"));
    }

    // The resurrection guard, one advisor write timestamp at a time: exactly
    // the fields any_fresh_advisor_file_is_not_retired covers.
    void a_restarted_advisor_loop_brings_the_panel_back() {
        const qint64 now = 1'800'000'000'000;
        const qint64 wrote = now - 28LL * 3'600'000;
        const qint64 fresh = now - 30'000;
        const QStringList fields{"loop.heartbeat_at_ms", "loop.updated_at_ms",
            "qualification.updated_at_ms", "promotion.updated_at_ms", "safety.updated_at_ms",
            "canary.updated_at_ms", "latest.opened_at_ms"};
        for (int i = 0; i < fields.size(); ++i) {
            QJsonObject loop = stale_loop(wrote);
            QJsonObject qualification{{"updated_at_ms",double(wrote)}};
            QJsonObject promotion{{"updated_at_ms",double(wrote)}};
            QJsonObject safety{{"updated_at_ms",double(wrote)}};
            QJsonObject canary{{"updated_at_ms",double(wrote)}};
            QJsonObject latest{{"opened_at_ms",double(wrote)}};
            switch (i) {
                case 0: loop["heartbeat_at_ms"] = double(fresh); break;
                case 1: loop["updated_at_ms"] = double(fresh); break;
                case 2: qualification["updated_at_ms"] = double(fresh); break;
                case 3: promotion["updated_at_ms"] = double(fresh); break;
                case 4: safety["updated_at_ms"] = double(fresh); break;
                case 5: canary["updated_at_ms"] = double(fresh); break;
                default: latest["opened_at_ms"] = double(fresh); break;
            }
            const auto card = present_concluded_duel_card(report_v5(),
                advisor_duel_retirement(loop,qualification,promotion,safety,canary,latest,now), now);
            QVERIFY2(card.presence == AdvisorDuelPresence::LiveAgain,
                     qPrintable(QStringLiteral("fresh %1 did not read LIVE AGAIN").arg(fields.at(i))));
            QVERIFY2(card.advisor_panel_reachable,
                     qPrintable(QStringLiteral("fresh %1 left the panel unreachable").arg(fields.at(i))));
            QVERIFY(card.state.contains("LIVE AGAIN"));
            QVERIFY(card.state.contains("ADVISOR & CANARY"));
        }
    }

    // The half `retired` alone cannot express. Both of these come back
    // retired == false, and reading that as a resurrection would put the dead
    // tab back on a machine with no duel to show.
    void absent_or_future_dated_advisor_state_is_not_a_resurrection() {
        const qint64 now = 1'800'000'000'000;
        const auto absent = advisor_duel_retirement({}, {}, {}, {}, {}, {}, now);
        QVERIFY(!absent.retired);
        QVERIFY(!absent.skewed);
        const auto absent_card = present_concluded_duel_card({}, absent, now);
        QCOMPARE(absent_card.presence, AdvisorDuelPresence::NoEvidence);
        QVERIFY(!absent_card.advisor_panel_reachable);
        QVERIFY(absent_card.state.contains("NO ADVISOR EVIDENCE"));
        QVERIFY(!absent_card.state.contains("LIVE AGAIN"));

        const auto skewed = advisor_duel_retirement(stale_loop(now - 28LL*3'600'000), {}, {}, {},
            QJsonObject{{"updated_at_ms",double(now+60'000)}}, {}, now);
        QVERIFY(!skewed.retired);
        QVERIFY(skewed.skewed);
        QCOMPARE(skewed.last_update_ms, now - 28LL*3'600'000);  // old, and still not live
        const auto skewed_card = present_concluded_duel_card(report_v5(), skewed, now);
        QCOMPARE(skewed_card.presence, AdvisorDuelPresence::NoEvidence);
        QVERIFY(!skewed_card.advisor_panel_reachable);
        QVERIFY(skewed_card.state.contains("clock skew"));
        QVERIFY(!skewed_card.state.contains("LIVE AGAIN"));
    }

    // Criterion: the record relocates whole. Verdict, epochs, settled counts
    // and the scoring freeze are on the card in every state — the report is on
    // disk whatever the loop is doing.
    void the_card_carries_the_whole_frozen_record_in_every_state() {
        const qint64 now = 1'800'000'000'000;
        const QList<AdvisorDuelRetirement> states{
            advisor_duel_retirement(stale_loop(now - 28LL*3'600'000), {}, {}, {}, {}, {}, now),
            advisor_duel_retirement(stale_loop(now - 30'000), {}, {}, {}, {}, {}, now),
            advisor_duel_retirement({}, {}, {}, {}, {}, {}, now)};
        for (const auto& retirement : states) {
            const auto card = present_concluded_duel_card(report_v5(), retirement, now);
            QVERIFY(card.record_available);
            QVERIFY(card.record.contains("NO WINNER DECLARED — INSUFFICIENT_PAIRED_DATA"));
            QVERIFY(card.record.contains("53 / 200 jointly resolved of 264 opportunities"));
            QVERIFY(card.record.contains("claude=kalshi-blind-claude-cli-v5-latency-neutral"));
            QVERIFY(card.record.contains("codex=kalshi-blind-codex-v4-zero-capability-latency-neutral"));
            QVERIFY(card.record.contains("SCORING FROZEN · infrastructure 2876e683b880"));
        }
    }

    // The count is quoted from competition_report.py's SCORING_INFRASTRUCTURE
    // tuple, which is also the charter's frozen-file list. It is a reference
    // to the manifest, and the card says so rather than printing a bare 7.
    void the_scoring_freeze_note_references_the_seven_file_manifest() {
        QCOMPARE(kAdvisorScoringManifestFiles, 7);
        const auto card = present_concluded_duel_card(report_v5(),
            advisor_duel_retirement(stale_loop(1'800'000'000'000 - 28LL*3'600'000), {}, {}, {}, {},
                                    {}, 1'800'000'000'000), 1'800'000'000'000);
        QVERIFY(card.record.contains("SHA-256 over the 7-file SCORING_INFRASTRUCTURE manifest"));
    }

    // A machine with no report still gets an honest card, not a blank one.
    void a_missing_report_still_states_the_duel_is_archived() {
        const qint64 now = 1'800'000'000'000;
        const auto card = present_concluded_duel_card({},
            advisor_duel_retirement(stale_loop(now - 28LL*3'600'000), {}, {}, {}, {}, {}, now), now);
        QCOMPARE(card.presence, AdvisorDuelPresence::Archived);
        QVERIFY(!card.record_available);
        QVERIFY(card.state.contains("ARCHIVED"));
        QVERIFY(card.record.contains("FINAL FROZEN DUEL RECORD UNAVAILABLE"));
        QVERIFY(!card.record.contains("WINNER"));
    }

  private:
    // The frozen report this machine actually holds, field for field.
    static QJsonObject report_v5() {
        return QJsonObject{{"result_state","INSUFFICIENT_PAIRED_DATA"},
            {"jointly_resolved",53}, {"opportunities",264},
            {"epoch_pair",QJsonObject{{"claude","kalshi-blind-claude-cli-v5-latency-neutral"},
                {"codex","kalshi-blind-codex-v4-zero-capability-latency-neutral"}}},
            {"coverage",QJsonObject{{"claude",0.25},{"codex",0.9848484848484849}}},
            {"thresholds",QJsonObject{{"minimum_jointly_resolved",200},{"minimum_coverage_each",0.8}}},
            {"scoring_infrastructure_hash",
             "2876e683b880cdc77c82a5dcc1e32662cd54188668434d51cdccb19d6c3b7a48"},
            {"shadow_only",true},{"execution_eligible",false}};
    }
};

QTEST_GUILESS_MAIN(AdvisorCanaryPresentationTest)
#include "tst_advisor_canary_presentation.moc"
