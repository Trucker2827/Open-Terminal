#include <QtTest>

#include "screens/kalshi/AdvisorCanaryPresentation.h"

using namespace openmarketterminal::screens::kalshi;

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
};

QTEST_GUILESS_MAIN(AdvisorCanaryPresentationTest)
#include "tst_advisor_canary_presentation.moc"
