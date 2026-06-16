#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include "services/observer/ObserverJournalService.h"

using openmarketterminal::services::ObserverJournalService;

// A fixture journal shaped like the real one: an intro, a manual block, two daily
// "(auto)" blocks (the second newest, one with an alert), and a weekly review block.
static const char* kJournal =
    "# Trade Journal\n\nLearning log.\n\n"
    "## 2026-06-15 (read #1 — baseline)\n- manual note\n\n"
    "## 2026-06-16 08:57 (auto)\n"
    "- **Snapshot** — BTC $65,000\n"
    "- **Regime** — BTC: chop (no edge)\n"
    "- _nothing notable_\n\n"
    "## 2026-06-17 08:57 (auto)\n"
    "- **Snapshot** — BTC $60,000\n"
    "- **Regime** — BTC: breakdown\n"
    "- **⚠️ ALERT:** BTC broke support $60,849 (now $60,000)\n\n"
    "## Week review 2026-06-21 (auto, READ-ONLY)\n"
    "- **Observed** — 5 days\n"
    "- **Net lesson** — no demonstrated edge\n\n"
    "<!-- Next daily read appends here -->\n";

static const char* kHistory =
    "{\"date\": \"2026-06-16\", \"leader\": \"ETH\", \"btc\": {\"price\": 65000, \"regime\": \"chop\"}}\n"
    "{\"date\": \"2026-06-17\", \"leader\": \"BTC\", \"btc\": {\"price\": 60000, \"regime\": \"breakdown\"}}\n";

class TstObserverJournal : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    static void writeFile(const QString& path, const char* body) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(body);
        f.close();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        writeFile(m_dir.filePath("trade-journal.md"), kJournal);
        writeFile(m_dir.filePath("observe_history.jsonl"), kHistory);
        ObserverJournalService::instance().setDirOverride(m_dir.path());
    }

    void available_true_with_fixture() {
        QVERIFY(ObserverJournalService::instance().available());
    }

    void unavailable_when_dir_missing() {
        auto& svc = ObserverJournalService::instance();
        const QString saved = svc.journalDir();
        svc.setDirOverride("/no/such/observer/dir");
        QVERIFY(!svc.available());
        QVERIFY(!svc.latestDaily().has_value());
        QVERIFY(svc.history().isEmpty());
        svc.setDirOverride(saved);              // restore for the remaining slots
    }

    void latest_daily_picks_newest_auto_block() {
        auto b = ObserverJournalService::instance().latestDaily();
        QVERIFY(b.has_value());
        QCOMPARE(b->kind, QString("daily"));
        QCOMPARE(b->title, QString("2026-06-17 08:57 (auto)"));   // newest, not the 06-16 one
        QVERIFY(b->markdown.contains("$60,000"));
        QVERIFY(!b->markdown.contains("$65,000"));                // bounded to its own block
    }

    void latest_weekly_picks_week_review_block() {
        auto b = ObserverJournalService::instance().latestWeekly();
        QVERIFY(b.has_value());
        QCOMPARE(b->kind, QString("weekly"));
        QVERIFY(b->markdown.contains("no demonstrated edge"));
        QVERIFY(!b->markdown.contains("Next daily read"));   // block must end at the marker, not absorb it
    }

    void recent_alerts_filters_and_extracts() {
        auto alerts = ObserverJournalService::instance().recentAlerts(10);
        QCOMPARE(alerts.size(), 1);                              // only the 06-17 block fired
        QCOMPARE(alerts[0].title, QString("2026-06-17 08:57 (auto)"));
        QVERIFY(alerts[0].alert.startsWith("BTC broke support"));   // markdown prefix stripped
    }

    void history_parses_jsonl_and_tails() {
        auto& svc = ObserverJournalService::instance();
        QCOMPARE(svc.history().size(), 2);
        auto last = svc.history(1);
        QCOMPARE(last.size(), 1);
        QCOMPARE(last[0].toObject().value("date").toString(), QString("2026-06-17"));
    }
};

QTEST_MAIN(TstObserverJournal)
#include "tst_observer_journal.moc"
