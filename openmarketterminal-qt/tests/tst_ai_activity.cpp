#include "storage/repositories/TradeAuditRepository.h"
#include "core/events/EventBus.h"
#include "core/headless/HeadlessRuntime.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QVariantMap>

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TestAiActivity : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    HeadlessRuntime rt_;
  private slots:
    void initTestCase();
    void cleanupTestCase();
    void appendPublishesTradeAuditEvent();
};

void TestAiActivity::initTestCase() {
    QVERIFY(home_.isValid());
    qputenv("HOME", home_.path().toUtf8());
    auto r = rt_.init("default");
    QVERIFY2(r.ok, qPrintable(r.error));
}
void TestAiActivity::cleanupTestCase() { rt_.shutdown(); }

void TestAiActivity::appendPublishesTradeAuditEvent() {
    QVariantMap got; int fires = 0;
    auto id = EventBus::instance().subscribe("trade.audit", [&](const QVariantMap& m){ got = m; ++fires; });
    TradeAuditRow row;
    row.ts = "2026-06-15T20:00:00Z"; row.phase = "fast"; row.tool = "fast_submit_order";
    row.account = "acct-1"; row.mode = "live"; row.decision = "filled";
    row.reason = "filled"; row.intent_json = R"({"symbol":"AAPL","side":"buy","quantity":1})";
    row.risk_snapshot_json = R"({"x":1})";
    auto wr = TradeAuditRepository::instance().append(row);
    QVERIFY2(wr.is_ok(), qPrintable(QString("append failed: %1").arg(QString::fromStdString(wr.error()))));
    QCOMPARE(fires, 1);
    QCOMPARE(got.value("ts").toString(),              QString("2026-06-15T20:00:00Z"));
    QCOMPARE(got.value("phase").toString(),           QString("fast"));
    QCOMPARE(got.value("tool").toString(),            QString("fast_submit_order"));
    QCOMPARE(got.value("account").toString(),         QString("acct-1"));
    QCOMPARE(got.value("mode").toString(),            QString("live"));
    QCOMPARE(got.value("intent_json").toString(),     QString(R"({"symbol":"AAPL","side":"buy","quantity":1})"));
    QCOMPARE(got.value("decision").toString(),        QString("filled"));
    QCOMPARE(got.value("reason").toString(),          QString("filled"));
    QCOMPARE(got.value("risk_snapshot_json").toString(), QString(R"({"x":1})"));
    EventBus::instance().unsubscribe(id);
}

QTEST_MAIN(TestAiActivity)
#include "tst_ai_activity.moc"
