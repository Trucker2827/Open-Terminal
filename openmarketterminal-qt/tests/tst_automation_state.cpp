#include <QtTest>
#include <QFile>
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
    void tail_read_finds_recent_candidate_in_large_file() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        // ~2 MB of filler no-trade rows, then one fresh candidate at the end.
        QString err;
        const QJsonObject filler{{"symbol", "BTC-USD"}, {"verdict", "NO TRADE"},
                                 {"action", "NO_ORDER"},
                                 {"pad", QString(400, QChar('x'))}};
        for (int i = 0; i < 4000; ++i)
            QVERIFY(append_jsonl(decisions_path("default"), filler, &err));
        const QJsonObject good{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(QDateTime::currentMSecsSinceEpoch())}};
        QVERIFY(append_jsonl(decisions_path("default"), good, &err));
        const QJsonObject c = latest_candidate("default", "BTC-USD", 15, &err);
        QVERIFY(!c.isEmpty());
        QCOMPARE(c.value("reference_price").toDouble(), 60000.0);
        // Behavioral proof of tail-read: a partial first line must not break parsing.
        const QByteArray tail = read_tail(decisions_path("default"), 1024);
        QVERIFY(!tail.startsWith('{') || tail.startsWith("{\""));  // starts at a line boundary
        QVERIFY(tail.endsWith("\n"));
        // Stronger boundary proof: the leading partial-line remnant from a
        // filler row is a run of 'x' padding, which does not happen to begin
        // with '{' -- so the two checks above pass even if the leading
        // partial line were left in. Require the first line of the tail to
        // parse as a complete JSON object, which a leftover fragment cannot.
        const QByteArray firstLine = tail.split('\n').constFirst();
        QVERIFY(!firstLine.isEmpty());
        QJsonParseError pe;
        const QJsonDocument firstDoc = QJsonDocument::fromJson(firstLine, &pe);
        QVERIFY2(pe.error == QJsonParseError::NoError && firstDoc.isObject(),
                 "tail must start at a complete, parseable JSON line, not a partial one");
    }
    void read_tail_small_file_returns_all() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QString err;
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"a", 1}}, &err));
        const QByteArray whole = read_tail(orders_path("default"), 512 * 1024);
        QCOMPARE(whole.count('\n'), 1);
    }
    void consumed_candidate_is_skipped() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QJsonObject cand{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", ts}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), cand, &err));
        QCOMPARE(candidate_key(cand), QStringLiteral("BTC-USD|") + ts);
        QVERIFY(!latest_candidate("default", "BTC-USD", 60, &err).isEmpty());
        QVERIFY(mark_consumed("default", candidate_key(cand), &err));
        QVERIFY(is_consumed("default", candidate_key(cand)));
        QVERIFY2(latest_candidate("default", "BTC-USD", 60, &err).isEmpty(),
                 "consumed candidate must not be returned again");
    }
    void spot_key_prefers_journal_id() {
        const QJsonObject spot{{"id", "abc-123"}, {"symbol", "BTC-USD"}, {"ts_ms", "1"}};
        QCOMPARE(candidate_key(spot), QStringLiteral("abc-123"));
    }
    void record_live_attempt_is_authoritative_over_scan() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QString err;
        // Explicitly empty orders jsonl -- a tail-scan-only implementation
        // would report 0 here no matter how chatty (or empty) the journal is.
        QFile orders(orders_path("default"));
        QVERIFY(orders.open(QIODevice::WriteOnly));
        orders.close();
        QVERIFY(record_live_attempt("default", &err));
        QVERIFY(record_live_attempt("default", &err));
        QCOMPARE(submitted_today_count("default"), 2);
    }
    void record_live_attempt_resets_on_new_day_and_scan_falls_back_when_stale() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        QString err;
        const QString yesterday = QDateTime::currentDateTimeUtc().addDays(-1).date().toString(Qt::ISODate);
        QVERIFY(write_json_object(daily_orders_path("default"),
                                  QJsonObject{{"date", yesterday}, {"count", 5}}, &err));
        const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QVERIFY(append_jsonl(orders_path("default"), QJsonObject{{"ts", ts}, {"submitted", true}}, &err));
        QCOMPARE(submitted_today_count("default"), 1);  // counter is stale (yesterday) -> falls back to tail scan
        QVERIFY(record_live_attempt("default", &err));
        const QJsonObject counter = read_json_object(daily_orders_path("default"));
        QCOMPARE(counter.value("count").toInt(), 1);
        QCOMPARE(counter.value("date").toString(),
                 QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate));
        QCOMPARE(submitted_today_count("default"), 1);  // now authoritative via the fresh counter
    }
};
QTEST_GUILESS_MAIN(TstAutomationState)
#include "tst_automation_state.moc"
