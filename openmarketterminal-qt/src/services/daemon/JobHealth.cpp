#include "services/daemon/JobHealth.h"

namespace openmarketterminal::services::daemon {

JobHealthClass classify_job_health(const JobRunHistory& history) {
    if (history.total_runs <= 0)
        return JobHealthClass::NoHistory;

    if (history.ok_runs <= 0) {
        // Never once succeeded. Below the floor this is a new job that has not
        // had a fair chance yet, so it stays a plain recent failure -- adding a
        // job must not immediately raise an alarm on its first bad run.
        return history.total_runs >= kNeverSucceededMinRuns
                   ? JobHealthClass::NeverSucceeded
                   : JobHealthClass::RecentFailure;
    }

    if (history.runs_since_success >= kChronicFailureStreak)
        return JobHealthClass::ChronicFailure;
    if (history.runs_since_success > 0)
        return JobHealthClass::RecentFailure;
    return JobHealthClass::Healthy;
}

QString job_health_class_label(JobHealthClass klass) {
    switch (klass) {
        case JobHealthClass::NoHistory:
            return QStringLiteral("no_history");
        case JobHealthClass::Healthy:
            return QStringLiteral("healthy");
        case JobHealthClass::RecentFailure:
            return QStringLiteral("recent_failure");
        case JobHealthClass::ChronicFailure:
            return QStringLiteral("chronic_failure");
        case JobHealthClass::NeverSucceeded:
            return QStringLiteral("never_succeeded");
    }
    return QStringLiteral("unknown");
}

bool job_health_needs_attention(JobHealthClass klass) {
    // A single recent failure is noise on a job that otherwise works -- the old
    // diagnosis treated it as the headline and that is exactly how a job with
    // zero lifetime successes hid among healthy ones.
    return klass == JobHealthClass::NeverSucceeded ||
           klass == JobHealthClass::ChronicFailure;
}

int job_health_severity(JobHealthClass klass) {
    switch (klass) {
        case JobHealthClass::NeverSucceeded:
            return 40;
        case JobHealthClass::ChronicFailure:
            return 30;
        case JobHealthClass::RecentFailure:
            return 20;
        case JobHealthClass::Healthy:
            return 10;
        case JobHealthClass::NoHistory:
            return 0;
    }
    return 0;
}

QString job_health_summary(const JobRunHistory& history) {
    if (history.total_runs <= 0)
        return QStringLiteral("no runs recorded");

    if (history.ok_runs <= 0) {
        return QStringLiteral("%1 runs, 0 ok — never succeeded")
            .arg(history.total_runs);
    }

    QString text = QStringLiteral("%1/%2 ok")
                       .arg(history.ok_runs)
                       .arg(history.total_runs);
    if (!history.last_success_at.isEmpty()) {
        text += QStringLiteral(", last success %1").arg(history.last_success_at);
        if (history.runs_since_success > 0) {
            text += QStringLiteral(" (%1 runs ago)").arg(history.runs_since_success);
        }
    }
    return text;
}

}  // namespace openmarketterminal::services::daemon
