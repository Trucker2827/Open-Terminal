// A job killed at its deadline is not a job that failed to start.
//
// The daemon's errorOccurred handler ignored its ProcessError argument and
// labelled every one "process start error". Its own timeout timer calls
// p->kill(), which raises Crashed -- so "Strategy sandbox tick" runs that had
// lasted the full ~45s timeout were filed as failures to SPAWN, a duration
// that is impossible for a process that never started.
//
// The handler also set daemon_history_recorded, suppressing the finished()
// handler that would have recorded the accurate `timeout`. The wrong label
// replaced the right one rather than sitting beside it.

#include <QtTest/QtTest>

#include "services/daemon/JobProcessError.h"

using namespace openmarketterminal::services::daemon;

class JobProcessErrorTest : public QObject {
    Q_OBJECT

private slots:
    // ---- who owns closing the run out ----------------------------------
    void only_failed_to_start_is_terminal() {
        // FailedToStart emits no finished(), so the error handler is the last
        // chance to close the run.
        QVERIFY(job_process_error_is_terminal(QProcess::FailedToStart));
    }

    void a_kill_is_not_terminal_because_finished_still_arrives() {
        // This is the production bug: p->kill() on timeout raises Crashed, and
        // finished() follows with the real verdict. Recording here overwrote it.
        QVERIFY(!job_process_error_is_terminal(QProcess::Crashed));
    }

    void no_other_error_is_terminal() {
        QVERIFY(!job_process_error_is_terminal(QProcess::Timedout));
        QVERIFY(!job_process_error_is_terminal(QProcess::WriteError));
        QVERIFY(!job_process_error_is_terminal(QProcess::ReadError));
        QVERIFY(!job_process_error_is_terminal(QProcess::UnknownError));
    }

    // ---- the label must not lie ----------------------------------------
    void a_crash_is_never_described_as_a_start_failure() {
        const QString s = job_process_error_label(QProcess::Crashed);
        QVERIFY2(!s.contains(QStringLiteral("start"), Qt::CaseInsensitive), qUtf8Printable(s));
        QVERIFY2(s.contains(QStringLiteral("terminated")), qUtf8Printable(s));
    }

    void a_start_failure_says_so() {
        const QString s = job_process_error_label(QProcess::FailedToStart);
        QVERIFY2(s.contains(QStringLiteral("failed to start")), qUtf8Printable(s));
    }

    void each_error_gets_its_own_wording() {
        QSet<QString> seen;
        for (auto e : {QProcess::FailedToStart, QProcess::Crashed, QProcess::Timedout,
                       QProcess::WriteError, QProcess::ReadError})
            seen.insert(job_process_error_label(e));
        QCOMPARE(seen.size(), 5);  // no two causes share a message
    }

    void the_os_detail_is_carried_through() {
        const QString s = job_process_error_label(QProcess::FailedToStart,
                                                  QStringLiteral("No such file or directory"));
        QVERIFY2(s.contains(QStringLiteral("No such file or directory")), qUtf8Printable(s));
        QVERIFY2(s.contains(QStringLiteral("failed to start")), qUtf8Printable(s));
    }

    void an_empty_detail_leaves_no_dangling_separator() {
        const QString s = job_process_error_label(QProcess::Crashed, QStringLiteral("   "));
        QVERIFY2(!s.endsWith(QStringLiteral(":")), qUtf8Printable(s));
        QCOMPARE(s, job_process_error_label(QProcess::Crashed));
    }

    void an_unknown_error_still_produces_something_readable() {
        const QString s = job_process_error_label(QProcess::UnknownError);
        QVERIFY(!s.isEmpty());
    }
};

QTEST_MAIN(JobProcessErrorTest)
#include "tst_job_process_error.moc"
