#include "storage/repositories/TradeAuditRepository.h"
#include "trading/ai_activity/AiActivityFormat.h"
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
    void formatToastsTerminalOutcomes();
    void formatNoToastForPrepareOrValidation();
    void formatSeverityMapping();
    void formatMessageAndMalformedIntent();
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

using openmarketterminal::trading::format_activity;
using Sev = openmarketterminal::trading::ActivityView::Severity;
static openmarketterminal::TradeAuditRow mkrow(const QString& phase, const QString& tool,
        const QString& decision, const QString& mode = "live", const QString& intent = "{}") {
    openmarketterminal::TradeAuditRow r; r.ts="2026-06-15T20:00:00Z"; r.phase=phase; r.tool=tool;
    r.decision=decision; r.mode=mode; r.intent_json=intent; r.reason=decision;
    r.risk_snapshot_json="{}"; return r;
}

void TestAiActivity::formatToastsTerminalOutcomes() {
    for (const char* d : {"filled","partially_filled","accepted","submitted","new","open",
                          "cancelled","canceled","rejected","denied"})
        QVERIFY2(format_activity(mkrow("fast","fast_submit_order",d)).toast, d);
    QVERIFY(format_activity(mkrow("submit","submit_order","FILLED")).toast);  // case-insensitive
}
void TestAiActivity::formatNoToastForPrepareOrValidation() {
    QVERIFY(!format_activity(mkrow("prepare","prepare_order","ok")).toast);
    QVERIFY(!format_activity(mkrow("prepare","prepare_order","filled")).toast);  // prepare never toasts
    for (const char* d : {"ok","draft","valid","prepared",""})
        QVERIFY2(!format_activity(mkrow("submit","submit_order",d)).toast, d);
}
void TestAiActivity::formatSeverityMapping() {
    QCOMPARE(format_activity(mkrow("fast","fast_submit_order","filled")).severity, Sev::Success);
    QCOMPARE(format_activity(mkrow("fast","cancel_order","cancelled")).severity, Sev::Success);
    QCOMPARE(format_activity(mkrow("fast","fast_submit_order","accepted")).severity, Sev::Info);
    QCOMPARE(format_activity(mkrow("submit","submit_order","rejected")).severity, Sev::Error);
    QCOMPARE(format_activity(mkrow("fast","fast_submit_order","denied")).severity, Sev::Error);
}
void TestAiActivity::formatMessageAndMalformedIntent() {
    auto v = format_activity(mkrow("fast","fast_submit_order","filled","live",
                                   R"({"symbol":"AAPL","side":"buy","quantity":1})"));
    QVERIFY(v.message.contains("fast_submit_order"));
    QVERIFY(v.message.contains("AAPL"));
    QVERIFY(v.message.contains("filled"));
    QVERIFY(v.message.contains("live"));
    auto v2 = format_activity(mkrow("fast","fast_submit_order","filled","live","not json"));
    QVERIFY(v2.message.contains("fast_submit_order"));  // malformed intent → safe fallback, no crash
}

QTEST_MAIN(TestAiActivity)
#include "tst_ai_activity.moc"
