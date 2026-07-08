// tst_portfolio_account_sync_migration.cpp — verifies the v060 migration
// adds the portfolio account-sync columns (sync_source, synced_at,
// sync_error on portfolios; has_cost_basis on portfolio_assets).
//
// Bring-up mirrors tst_pm_paper.cpp: a HeadlessRuntime init("default")
// registers all migrations (incl. v060) and opens the DB under a
// QTemporaryDir HOME before any repo call.

#include "core/headless/HeadlessRuntime.h"
#include "storage/sqlite/Database.h"
#include <QtTest>
#include <QTemporaryDir>
using namespace openmarketterminal;
class TstAccountSyncMigration : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    headless::HeadlessRuntime rt_;
  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }
    void columnsExist() {
        struct C { const char* table; const char* col; };
        for (auto c : {C{"portfolios","sync_source"}, C{"portfolios","synced_at"},
                       C{"portfolios","sync_error"}, C{"portfolio_assets","has_cost_basis"}}) {
            auto r = Database::instance().execute(QString("PRAGMA table_info(%1)").arg(c.table));
            QVERIFY2(r.is_ok(), c.table);
            bool found = false;
            auto q = r.value();
            while (q.next()) if (q.value(1).toString() == c.col) found = true;
            QVERIFY2(found, qPrintable(QString("%1.%2 missing").arg(c.table, c.col)));
        }
    }
};
// NOTE: QTEST_APPLESS_MAIN (as suggested by the brief snippet) crashes with
// SIGSEGV — QSqlDatabase requires a QCoreApplication. QTEST_MAIN mirrors
// tst_pm_paper.cpp / tst_order_flow.cpp, the reference bring-up pattern.
QTEST_MAIN(TstAccountSyncMigration)
#include "tst_portfolio_account_sync_migration.moc"
