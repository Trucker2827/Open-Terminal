// tst_freshness_gate.cpp — the extracted pure freshness/data-quality helper.
#include "services/sandbox/FreshnessGate.h"

#include <QtTest/QtTest>
#include <QJsonObject>

using namespace openmarketterminal::services::sandbox;

class TstFreshnessGate : public QObject {
    Q_OBJECT
  private slots:
    void three_way_classification() {
        QCOMPARE(data_quality_from_freshness(QJsonObject{}), QStringLiteral("unknown"));
        QCOMPARE(data_quality_from_freshness(QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 3}}),
                 QStringLiteral("ok"));
        QCOMPARE(data_quality_from_freshness(QJsonObject{{"freshest_age_ms", 9000}, {"live_sources", 3}}),
                 QStringLiteral("degraded")); // stale
        QCOMPARE(data_quality_from_freshness(QJsonObject{{"freshest_age_ms", 100}, {"live_sources", 1}}),
                 QStringLiteral("degraded")); // too few sources
        QCOMPARE(data_quality_from_freshness(QJsonObject{{"live_sources", 1}}),
                 QStringLiteral("degraded")); // age absent, sources<2
    }
};

QTEST_GUILESS_MAIN(TstFreshnessGate)
#include "tst_freshness_gate.moc"
