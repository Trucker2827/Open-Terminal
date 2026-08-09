// A job run's verdict is decided ONCE. Two independent paths finalise a run --
// reconcile_stale_running_jobs() writing `stale-timeout`, and the QProcess
// `finished` handler writing the real exit -- and they do not coordinate.
//
// Found in production: only 36 `stale-timeout` rows survived since Aug 1 while
// 1739+ runs had outlived their timeout. The second write was erasing the
// first, and since duration is recomputed from the row's original started_at,
// rows read `ok` with durations of many minutes against a 45s timeout. The
// health surface then reported those jobs as having met their deadline.

#include <QtTest/QtTest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "services/daemon/JobRunFinish.h"

using namespace openmarketterminal::services::daemon;

class JobRunFinishTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    QSqlDatabase db_;

    void finalise(const QString& run_id, const QString& status, int exit_code,
                  qint64 duration_ms, int* affected = nullptr) {
        QSqlQuery q(db_);
        QVERIFY2(q.prepare(job_run_finish_sql()), qUtf8Printable(q.lastError().text()));
        q.bindValue(":status", status);
        q.bindValue(":exit_code", exit_code);
        q.bindValue(":finished_at", QStringLiteral("2026-08-09T12:00:00.000Z"));
        q.bindValue(":duration_ms", duration_ms);
        q.bindValue(":stdout_tail", QString());
        q.bindValue(":stderr_tail", QString());
        q.bindValue(":error", QString());
        q.bindValue(":run_id", run_id);
        QVERIFY2(q.exec(), qUtf8Printable(q.lastError().text()));
        if (affected)
            *affected = q.numRowsAffected();
    }

    QString status_of(const QString& run_id) {
        QSqlQuery q(db_);
        q.prepare("SELECT status FROM daemon_job_runs WHERE run_id=:r");
        q.bindValue(":r", run_id);
        return (q.exec() && q.next()) ? q.value(0).toString() : QString();
    }

    qint64 duration_of(const QString& run_id) {
        QSqlQuery q(db_);
        q.prepare("SELECT duration_ms FROM daemon_job_runs WHERE run_id=:r");
        q.bindValue(":r", run_id);
        return (q.exec() && q.next()) ? q.value(0).toLongLong() : -1;
    }

    void open_run(const QString& run_id) {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO daemon_job_runs (run_id, job_id, job_name, status, started_at) "
                  "VALUES (:r, 'job_x', 'Strategy sandbox tick', 'running', "
                  "'2026-08-09T11:30:00.000Z')");
        q.bindValue(":r", run_id);
        QVERIFY2(q.exec(), qUtf8Printable(q.lastError().text()));
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        db_ = QSqlDatabase::addDatabase("QSQLITE", "tst_job_run_finish");
        db_.setDatabaseName(dir_.path() + "/runs.sqlite");
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        QVERIFY2(q.exec("CREATE TABLE daemon_job_runs (run_id TEXT PRIMARY KEY, job_id TEXT, "
                        "job_name TEXT, status TEXT, exit_code INTEGER, started_at TEXT, "
                        "finished_at TEXT, duration_ms INTEGER, stdout_tail TEXT, "
                        "stderr_tail TEXT, error TEXT)"),
                 qUtf8Printable(q.lastError().text()));
    }

    void cleanupTestCase() {
        db_.close();
        QSqlDatabase::removeDatabase("tst_job_run_finish");
    }

    // ---- the production bug -------------------------------------------
    void a_late_ok_must_not_erase_a_recorded_timeout() {
        open_run("run_a");
        finalise("run_a", "stale-timeout", -1, 60000);   // reconciler, at t+60s
        finalise("run_a", "ok", 0, 1398000);             // real exit, 23 min later
        QCOMPARE(status_of("run_a"), QStringLiteral("stale-timeout"));
        QCOMPARE(duration_of("run_a"), 60000LL);
    }

    void the_second_write_reports_that_it_changed_nothing() {
        open_run("run_b");
        int first = -1, second = -1;
        finalise("run_b", "timeout", -1, 45000, &first);
        finalise("run_b", "ok", 0, 900000, &second);
        QCOMPARE(first, 1);
        QCOMPARE(second, 0);  // caller can detect it lost the race
    }

    // ---- the normal path must be untouched ----------------------------
    void the_first_finalisation_still_writes_normally() {
        open_run("run_c");
        finalise("run_c", "ok", 0, 12400);
        QCOMPARE(status_of("run_c"), QStringLiteral("ok"));
        QCOMPARE(duration_of("run_c"), 12400LL);
    }

    void an_ok_run_is_not_later_downgraded_either() {
        // Symmetry: whoever gets there first owns the verdict, in both
        // directions. A stray late write must never rewrite a finished run.
        open_run("run_d");
        finalise("run_d", "ok", 0, 12400);
        finalise("run_d", "stale-timeout", -1, 60000);
        QCOMPARE(status_of("run_d"), QStringLiteral("ok"));
        QCOMPARE(duration_of("run_d"), 12400LL);
    }

    void an_unknown_run_id_affects_nothing() {
        int affected = -1;
        finalise("run_missing", "ok", 0, 1, &affected);
        QCOMPARE(affected, 0);
    }
};

QTEST_MAIN(JobRunFinishTest)
#include "tst_job_run_finish.moc"
