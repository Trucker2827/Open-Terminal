// tst_portfolio_trade_history_migration.cpp — verifies the v061 migration
// adds portfolio_transactions.external_id (used by
// PortfolioRepository::import_transaction's dedup path — see
// idx_ptx_external, a partial unique index on (portfolio_id, external_id)
// WHERE external_id != '').
//
// Bring-up mirrors tst_portfolio_account_sync_migration.cpp: a
// HeadlessRuntime init("default") registers all migrations (incl. v061) and
// opens the DB under a QTemporaryDir HOME before any repo call.

#include "core/headless/HeadlessRuntime.h"
#include "storage/sqlite/Database.h"
#include <QtTest>
#include <QTemporaryDir>
using namespace openmarketterminal;
class TstTradeHistoryMigration : public QObject {
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
    void externalIdColumnExists() {
        auto r = Database::instance().execute("PRAGMA table_info(portfolio_transactions)");
        QVERIFY2(r.is_ok(), "portfolio_transactions");
        bool found = false;
        auto q = r.value();
        while (q.next()) if (q.value(1).toString() == "external_id") found = true;
        QVERIFY2(found, "portfolio_transactions.external_id missing");
    }
};
// NOTE: QTEST_APPLESS_MAIN crashes with SIGSEGV — QSqlDatabase requires a
// QCoreApplication. QTEST_MAIN mirrors tst_portfolio_account_sync_migration.cpp.
QTEST_MAIN(TstTradeHistoryMigration)
#include "tst_portfolio_trade_history_migration.moc"
