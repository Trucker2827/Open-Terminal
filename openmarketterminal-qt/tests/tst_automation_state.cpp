#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "cli/automation/AutomationState.h"

using namespace openmarketterminal::cli::automation;

class TstAutomationState : public QObject {
    Q_OBJECT
  private slots:
    void guard_roundtrip() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QJsonObject guard{{"enabled", true}, {"max_order_usd", 100.0}};
        QString err;
        QVERIFY(write_json_object(live_guard_path(QStringLiteral("default")), guard, &err));
        const QJsonObject back = read_json_object(live_guard_path(QStringLiteral("default")));
        QCOMPARE(back.value("enabled").toBool(), true);
        QCOMPARE(back.value("max_order_usd").toDouble(), 100.0);
    }
    void latest_candidate_reads_fixture() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject good{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(now_ms)}};
        const QJsonObject stale{{"symbol", "BTC-USD"},
                                {"verdict", "PAPER TRADE CANDIDATE"},
                                {"action", "PAPER_LIMIT_BUY_ONLY"},
                                {"reference_price", 59000.0},
                                {"ts_ms", QString::number(now_ms - 3600 * 1000)}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), stale, &err));
        QVERIFY(append_jsonl(decisions_path("default"), good, &err));
        const QJsonObject c = latest_candidate("default", "BTC-USD", 15, &err);
        QVERIFY(!c.isEmpty());
        QCOMPARE(c.value("reference_price").toDouble(), 60000.0);
        // symbol filter must exclude
        QVERIFY(latest_candidate("default", "ETH-USD", 15, &err).isEmpty());
    }
    void submitted_today_counts_only_submitted() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString err;
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"ts", ts}, {"submitted", true}}, &err));
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"ts", ts}, {"dry_run", true}}, &err));
        QCOMPARE(submitted_today_count("default"), 1);
    }
    void profile_isolation() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(write_json_object(live_guard_path("botlab"), QJsonObject{{"enabled", true}}, nullptr));
        // default profile must NOT see botlab's guard
        QVERIFY(read_json_object(live_guard_path("default")).isEmpty());
        QVERIFY(live_guard_path("botlab").contains(QStringLiteral("/profiles/botlab/daemon/")));
    }
};
QTEST_GUILESS_MAIN(TstAutomationState)
#include "tst_automation_state.moc"
