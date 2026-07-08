// tst_portfolio_import_transaction.cpp — Trade-history sync: verifies
// PortfolioRepository::import_transaction dedups on (portfolio_id,
// external_id) via v061's idx_ptx_external partial unique index + INSERT OR
// IGNORE. Covers:
//   1. Importing the same external_id twice for the same portfolio inserts
//      ONE row (re-sync must not duplicate).
//   2. A different external_id inserts a second, distinct row.
//   3. Manual transactions (external_id == '', via add_transaction) are
//      unconstrained — multiple manual rows never collide with each other
//      or with a synced row.
//
// Bring-up mirrors tst_portfolio_account_sync_migration.cpp: a
// HeadlessRuntime init("default") registers all migrations (incl. v061) and
// opens the DB under a QTemporaryDir HOME before any repo call.

#include <QtTest>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "storage/repositories/PortfolioRepository.h"

using namespace openmarketterminal;

class TstPortfolioImportTransaction : public QObject {
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

    void same_external_id_imported_twice_dedups_to_one_row() {
        auto& repo = PortfolioRepository::instance();
        auto created = repo.create_portfolio("Dedup Test", "", "USD");
        QVERIFY2(created.is_ok(), created.is_err() ? created.error().c_str() : "");
        const QString pid = created.value();

        auto r1 = repo.import_transaction(pid, "AAPL", "BUY", 10, 150.0, "2026-01-01", "broker:order-1");
        QVERIFY2(r1.is_ok(), r1.is_err() ? r1.error().c_str() : "");

        auto r2 = repo.import_transaction(pid, "AAPL", "BUY", 10, 150.0, "2026-01-01", "broker:order-1");
        QVERIFY2(r2.is_ok(), r2.is_err() ? r2.error().c_str() : "");

        auto txs = repo.get_transactions(pid);
        QVERIFY(txs.is_ok());
        QCOMPARE(txs.value().size(), qsizetype{1});
        QCOMPARE(txs.value().first().symbol, QStringLiteral("AAPL"));
        QCOMPARE(txs.value().first().quantity, 10.0);
        QCOMPARE(txs.value().first().total_value, 1500.0);
    }

    void different_external_id_inserts_a_second_row() {
        auto& repo = PortfolioRepository::instance();
        auto created = repo.create_portfolio("Dedup Test 2", "", "USD");
        QVERIFY2(created.is_ok(), created.is_err() ? created.error().c_str() : "");
        const QString pid = created.value();

        auto r1 = repo.import_transaction(pid, "AAPL", "BUY", 10, 150.0, "2026-01-01", "broker:order-1");
        QVERIFY2(r1.is_ok(), r1.is_err() ? r1.error().c_str() : "");
        auto r2 = repo.import_transaction(pid, "MSFT", "SELL", 5, 300.0, "2026-01-02", "broker:order-2");
        QVERIFY2(r2.is_ok(), r2.is_err() ? r2.error().c_str() : "");

        auto txs = repo.get_transactions(pid);
        QVERIFY(txs.is_ok());
        QCOMPARE(txs.value().size(), qsizetype{2});
    }

    void manual_transactions_stay_unconstrained() {
        // Manual entries (add_transaction) always write external_id='',
        // which idx_ptx_external excludes from uniqueness (WHERE external_id
        // != ''). Two manual BUYs with otherwise-identical fields must NOT
        // collide with each other.
        auto& repo = PortfolioRepository::instance();
        auto created = repo.create_portfolio("Manual Unconstrained", "", "USD");
        QVERIFY2(created.is_ok(), created.is_err() ? created.error().c_str() : "");
        const QString pid = created.value();

        auto r1 = repo.add_transaction(pid, "AAPL", "BUY", 10, 150.0, "2026-01-01");
        QVERIFY2(r1.is_ok(), r1.is_err() ? r1.error().c_str() : "");
        auto r2 = repo.add_transaction(pid, "AAPL", "BUY", 10, 150.0, "2026-01-01");
        QVERIFY2(r2.is_ok(), r2.is_err() ? r2.error().c_str() : "");

        auto txs = repo.get_transactions(pid);
        QVERIFY(txs.is_ok());
        QCOMPARE(txs.value().size(), qsizetype{2});
    }
};

QTEST_MAIN(TstPortfolioImportTransaction)
#include "tst_portfolio_import_transaction.moc"
