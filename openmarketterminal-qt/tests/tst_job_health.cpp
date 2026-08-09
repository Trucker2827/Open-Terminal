// tst_job_health — a job that has NEVER worked must not read like one that
// just had a hiccup.
//
// Every case below is a real row from the operator's daemon_job_runs table on
// 2026-08-09. The old diagnosis put the first two in the same bucket and gave
// the healthy one the bigger fail_count.

#include <QtTest/QtTest>

#include "services/daemon/JobHealth.h"

using namespace openmarketterminal::services::daemon;

namespace {

// "collector crypto universe scorer" — 27048 runs, 19389 ok, last success
// minutes ago, and it happens to have just failed. Healthy.
JobRunHistory healthy_scorer() {
    return {27048, 19389, QStringLiteral("2026-08-09T18:58:36.614Z"), 1};
}

// "Bitcoin hourly event-impact scorer" — 180 runs since Aug 1, not one success.
// Its producer raises TypeError on a missing ANTHROPIC_API_KEY every hour.
JobRunHistory never_worked_event_impact() {
    return {180, 0, QString(), 180};
}

// "Kalshi daily auto cockpit plan" — 1761 runs, 0 successes.
JobRunHistory never_worked_cockpit_plan() {
    return {1761, 0, QString(), 1761};
}

// "Kalshi event-engine recovery watchdog" — 5412 runs, 1283 ok, but the last
// success was 2026-07-20. It does work; it has not worked in a long time.
JobRunHistory chronic_watchdog() {
    return {5412, 1283, QStringLiteral("2026-07-20T03:21:24.911Z"), 4129};
}

}  // namespace

class JobHealthTest : public QObject {
    Q_OBJECT

private slots:
    // ---- the distinction the old diagnosis could not draw --------------
    void never_succeeded_is_not_the_same_as_recently_failed() {
        // Both jobs' most recent run failed -- that is the whole trap. The
        // scorer failed once after 19389 successes; the event-impact job has
        // failed 180 times out of 180. They must not land in the same class,
        // and only one of them may raise an alarm.
        const JobHealthClass broken = classify_job_health(never_worked_event_impact());
        const JobHealthClass scorer = classify_job_health(healthy_scorer());
        QCOMPARE(broken, JobHealthClass::NeverSucceeded);
        QCOMPARE(scorer, JobHealthClass::RecentFailure);
        QVERIFY(broken != scorer);
        QVERIFY(job_health_needs_attention(broken));
        QVERIFY(!job_health_needs_attention(scorer));
    }

    void a_job_whose_last_run_succeeded_is_healthy() {
        const JobRunHistory clean{27048, 19389, QStringLiteral("2026-08-09T18:58:36.614Z"), 0};
        QCOMPARE(classify_job_health(clean), JobHealthClass::Healthy);
    }

    void a_huge_fail_count_does_not_by_itself_mean_unhealthy() {
        // The healthy scorer has 7659 failures -- 42x the event-impact job's
        // 180 -- and is fine. Volume of failures is not the signal; absence of
        // successes is.
        const JobRunHistory healthy = healthy_scorer();
        const JobRunHistory broken = never_worked_event_impact();
        QVERIFY(healthy.total_runs - healthy.ok_runs > broken.total_runs);
        QVERIFY(!job_health_needs_attention(classify_job_health(healthy)));
        QVERIFY(job_health_needs_attention(classify_job_health(broken)));
    }

    void the_1761_run_job_that_never_worked_is_flagged() {
        QCOMPARE(classify_job_health(never_worked_cockpit_plan()),
                 JobHealthClass::NeverSucceeded);
        QVERIFY(job_health_needs_attention(JobHealthClass::NeverSucceeded));
    }

    // ---- a job that used to work but stopped ---------------------------
    void long_streak_since_last_success_is_chronic() {
        QCOMPARE(classify_job_health(chronic_watchdog()), JobHealthClass::ChronicFailure);
        QVERIFY(job_health_needs_attention(JobHealthClass::ChronicFailure));
    }

    void a_short_streak_is_only_a_recent_failure() {
        // Succeeded 2 runs ago: noise, not an outage. Must not page anyone.
        const JobRunHistory blip{500, 480, QStringLiteral("2026-08-09T18:00:00.000Z"), 2};
        QCOMPARE(classify_job_health(blip), JobHealthClass::RecentFailure);
        QVERIFY(!job_health_needs_attention(JobHealthClass::RecentFailure));
    }

    void the_chronic_boundary_is_exact() {
        JobRunHistory h{500, 480, QStringLiteral("2026-08-09T18:00:00.000Z"),
                        kChronicFailureStreak - 1};
        QCOMPARE(classify_job_health(h), JobHealthClass::RecentFailure);
        h.runs_since_success = kChronicFailureStreak;
        QCOMPARE(classify_job_health(h), JobHealthClass::ChronicFailure);
    }

    // ---- benefit of the doubt for a brand-new job ----------------------
    void a_job_that_never_ran_reports_no_history() {
        QCOMPARE(classify_job_health(JobRunHistory{}), JobHealthClass::NoHistory);
        QVERIFY(!job_health_needs_attention(JobHealthClass::NoHistory));
    }

    void one_failed_run_is_too_early_to_call_it_broken() {
        // A newly added job must not be branded never-succeeded on run 1.
        const JobRunHistory fresh{1, 0, QString(), 1};
        QCOMPARE(classify_job_health(fresh), JobHealthClass::RecentFailure);
    }

    void the_never_succeeded_floor_is_exact() {
        JobRunHistory h{kNeverSucceededMinRuns - 1, 0, QString(),
                        kNeverSucceededMinRuns - 1};
        QCOMPARE(classify_job_health(h), JobHealthClass::RecentFailure);
        h.total_runs = kNeverSucceededMinRuns;
        h.runs_since_success = kNeverSucceededMinRuns;
        QCOMPARE(classify_job_health(h), JobHealthClass::NeverSucceeded);
    }

    // ---- ranking: the broken job must not be buried --------------------
    void never_succeeded_outranks_every_other_class() {
        QVERIFY(job_health_severity(JobHealthClass::NeverSucceeded) >
                job_health_severity(JobHealthClass::ChronicFailure));
        QVERIFY(job_health_severity(JobHealthClass::ChronicFailure) >
                job_health_severity(JobHealthClass::RecentFailure));
        QVERIFY(job_health_severity(JobHealthClass::RecentFailure) >
                job_health_severity(JobHealthClass::Healthy));
        QVERIFY(job_health_severity(JobHealthClass::Healthy) >=
                job_health_severity(JobHealthClass::NoHistory));
    }

    // ---- labels and summary --------------------------------------------
    void labels_are_distinct_and_stable() {
        const QList<JobHealthClass> all{
            JobHealthClass::NoHistory, JobHealthClass::Healthy,
            JobHealthClass::RecentFailure, JobHealthClass::ChronicFailure,
            JobHealthClass::NeverSucceeded};
        QSet<QString> seen;
        for (const JobHealthClass k : all) {
            const QString label = job_health_class_label(k);
            QVERIFY2(!label.isEmpty(), "every class needs a label");
            seen.insert(label);
        }
        QCOMPARE(seen.size(), all.size());
        QCOMPARE(job_health_class_label(JobHealthClass::NeverSucceeded),
                 QStringLiteral("never_succeeded"));
    }

    void summary_states_the_success_count_for_a_broken_job() {
        const QString s = job_health_summary(never_worked_event_impact());
        QVERIFY2(s.contains(QStringLiteral("180")), qUtf8Printable(s));
        QVERIFY2(s.contains(QStringLiteral("0")), qUtf8Printable(s));
        // The operator must be able to read "never" without doing arithmetic.
        QVERIFY2(s.contains(QStringLiteral("never"), Qt::CaseInsensitive),
                 qUtf8Printable(s));
    }

    void summary_names_the_last_success_for_a_chronic_job() {
        const QString s = job_health_summary(chronic_watchdog());
        QVERIFY2(s.contains(QStringLiteral("2026-07-20")), qUtf8Printable(s));
        QVERIFY2(s.contains(QStringLiteral("4129")), qUtf8Printable(s));
    }
};

QTEST_MAIN(JobHealthTest)
#include "tst_job_health.moc"
