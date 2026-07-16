// tst_migration_runner.cpp — reproduces the MigrationRunner high-water-mark
// skip bug directly against synthetic migrations, with no dependency on the
// real migration set (this binary never calls register_all_migrations(), so
// MigrationRunner's global registry starts empty for each process run).
//
// Bug: run() used to decide "already applied" via
// `m.version <= read_current_version()` (MAX(version) in schema_version).
// That means a migration REGISTERED LATER with a version number BELOW the
// current max is permanently skipped, even though it was never applied —
// breaking parallel branches that register out-of-order version numbers.
//
// Fix: skip iff the version is a member of the set of already-recorded rows
// in schema_version (read_applied_versions()), not iff version <= MAX.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal;

namespace {
Migration mk(int v) {
    return Migration{v, QString("m%1").arg(v), [v](QSqlDatabase& d) -> Result<void> {
                          QSqlQuery q(d);
                          if (!q.exec(QString("CREATE TABLE marker_%1(x INTEGER)").arg(v)))
                              return Result<void>::err(q.lastError().text().toStdString());
                          return Result<void>::ok();
                      }};
}

bool table_exists(QSqlDatabase& db, const QString& name) {
    QSqlQuery q(db);
    q.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    q.addBindValue(name);
    if (!q.exec())
        return false;
    return q.next();
}
} // namespace

class TstMigrationRunner : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QSqlDatabase db_;

  private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        db_ = QSqlDatabase::addDatabase("QSQLITE", "migtest");
        db_.setDatabaseName(dir_.path() + "/mig.db");
        QVERIFY(db_.open());
    }

    void cleanupTestCase() {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase("migtest");
    }

    // Reproduces the bug: a version registered AFTER an earlier run, whose
    // number is below the max already applied, must still be applied.
    void later_registered_lower_version_is_applied() {
        MigrationRunner::register_migration(mk(10));
        MigrationRunner::register_migration(mk(30));

        MigrationRunner runner(db_);
        QVERIFY2(runner.run().is_ok(), "first run (v10, v30) should succeed");
        QVERIFY2(table_exists(db_, "marker_10"), "marker_10 should exist after first run");
        QVERIFY2(table_exists(db_, "marker_30"), "marker_30 should exist after first run");
        QCOMPARE(runner.current_version(), 30);

        // Now register v20 — a version below the current max (30). Under the
        // OLD high-water-mark logic (m.version <= current), 20 <= 30 so it
        // would be permanently skipped. Under the fix (membership in
        // schema_version), it should be applied because it was never
        // recorded.
        MigrationRunner::register_migration(mk(20));

        MigrationRunner runner2(db_);
        QVERIFY2(runner2.run().is_ok(), "second run (adding v20) should succeed");
        QVERIFY2(table_exists(db_, "marker_20"),
                  "marker_20 should exist — later-registered lower version must be applied");
    }

    // Depends on later_registered_lower_version_is_applied having run first
    // (QtTest runs slots in declaration order) so schema_version already has
    // rows for {10, 20, 30}. A third run with no new registrations must be a
    // clean no-op: no duplicate record_version PRIMARY KEY violation, and no
    // extra rows.
    void already_applied_versions_are_not_reapplied() {
        MigrationRunner runner3(db_);
        QVERIFY2(runner3.run().is_ok(), "third run with nothing new should succeed with no errors");

        QSqlQuery q(db_);
        QVERIFY(q.exec("SELECT COUNT(*) FROM schema_version"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 3);
    }
};

QTEST_MAIN(TstMigrationRunner)
#include "tst_migration_runner.moc"
