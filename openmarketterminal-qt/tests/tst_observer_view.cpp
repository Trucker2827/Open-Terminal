#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "screens/observer/ObserverJournalView.h"
#include "services/observer/ObserverJournalService.h"

using openmarketterminal::screens::observer::JournalView;
using openmarketterminal::screens::observer::markdown_for;
using openmarketterminal::services::ObserverJournalService;

static const char* kJournal =
    "# Trade Journal\n\n"
    "## 2026-06-17 08:57 (auto)\n"
    "- **Snapshot** — BTC $60,000\n"
    "- **⚠️ ALERT:** BTC broke support $60,849\n\n"
    "## Week review 2026-06-21 (auto, READ-ONLY)\n"
    "- **Net lesson** — no demonstrated edge\n\n"
    "<!-- Next daily read appends here -->\n";

class TstObserverView : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        QFile f(m_dir.filePath("trade-journal.md"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(kJournal);
        f.close();
        ObserverJournalService::instance().setDirOverride(m_dir.path());
    }

    void latest_view_is_daily_block() {
        const QString md = markdown_for(JournalView::Latest, ObserverJournalService::instance());
        QVERIFY(md.contains("$60,000"));
        QVERIFY(!md.contains("no demonstrated edge"));   // not the weekly block
    }

    void weekly_view_is_review_block() {
        const QString md = markdown_for(JournalView::Weekly, ObserverJournalService::instance());
        QVERIFY(md.contains("no demonstrated edge"));
    }

    void alerts_view_lists_alerts() {
        const QString md = markdown_for(JournalView::Alerts, ObserverJournalService::instance());
        QVERIFY(md.contains("Recent alerts"));
        QVERIFY(md.contains("BTC broke support"));
        QVERIFY(md.contains("2026-06-17 08:57 (auto)"));
    }

    void missing_journal_gives_friendly_message() {
        auto& svc = ObserverJournalService::instance();
        const QString saved = svc.journalDir();
        svc.setDirOverride("/no/such/observer/dir");
        const QString md = markdown_for(JournalView::Latest, svc);
        QVERIFY(md.contains("not found"));
        QVERIFY(md.contains("OPENTERMINAL_OBSERVER_DIR"));
        svc.setDirOverride(saved);
    }
};

QTEST_MAIN(TstObserverView)
#include "tst_observer_view.moc"
