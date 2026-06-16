#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>

#include "mcp/tools/ObserverTools.h"
#include "services/observer/ObserverJournalService.h"

using openmarketterminal::mcp::ToolDef;
using openmarketterminal::mcp::ToolResult;
using openmarketterminal::mcp::tools::get_observer_tools;
using openmarketterminal::services::ObserverJournalService;

static const char* kJournal =
    "# Trade Journal\n\n"
    "## 2026-06-17 08:57 (auto)\n"
    "- **Snapshot** — BTC $60,000\n"
    "- **⚠️ ALERT:** BTC broke support $60,849\n\n"
    "## Week review 2026-06-21 (auto, READ-ONLY)\n"
    "- **Net lesson** — no demonstrated edge\n\n"
    "<!-- Next daily read appends here -->\n";

class TstObserverTools : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    static ToolDef tool() {
        auto tools = get_observer_tools();
        Q_ASSERT(tools.size() == 1);
        return tools.front();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        QFile f(m_dir.filePath("trade-journal.md"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(kJournal);
        f.close();
        ObserverJournalService::instance().setDirOverride(m_dir.path());
    }

    void exposes_one_readonly_tool() {
        const auto t = tool();
        QCOMPARE(t.name, QString("get_observations"));
        QVERIFY(!t.is_destructive);                              // read-only
    }

    void latest_view_returns_daily_block() {
        const ToolResult r = tool().handler(QJsonObject{{"view", "latest"}});
        QVERIFY(r.success);
        const QJsonObject d = r.data.toObject();
        QCOMPARE(d.value("view").toString(), QString("latest"));
        QVERIFY(d.value("markdown").toString().contains("$60,000"));
        QVERIFY(d.value("alert").toString().startsWith("BTC broke support"));
    }

    void default_view_is_latest() {
        const ToolResult r = tool().handler(QJsonObject{});      // no args
        QVERIFY(r.success);
        QCOMPARE(r.data.toObject().value("view").toString(), QString("latest"));
    }

    void week_view_returns_review() {
        const ToolResult r = tool().handler(QJsonObject{{"view", "week"}});
        QVERIFY(r.success);
        QVERIFY(r.data.toObject().value("markdown").toString().contains("no demonstrated edge"));
    }

    void alerts_view_returns_array() {
        const ToolResult r = tool().handler(QJsonObject{{"view", "alerts"}});
        QVERIFY(r.success);
        const QJsonArray a = r.data.toObject().value("alerts").toArray();
        QCOMPARE(a.size(), 1);
        QVERIFY(a[0].toObject().value("alert").toString().startsWith("BTC broke support"));
    }

    void unavailable_fails_cleanly() {
        auto& svc = ObserverJournalService::instance();
        const QString saved = svc.journalDir();
        svc.setDirOverride("/no/such/observer/dir");
        const ToolResult r = tool().handler(QJsonObject{{"view", "latest"}});
        QVERIFY(!r.success);
        QVERIFY(r.error.contains("not found"));
        svc.setDirOverride(saved);
    }
};

QTEST_MAIN(TstObserverTools)
#include "tst_observer_tools.moc"
