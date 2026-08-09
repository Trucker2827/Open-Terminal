#pragma once

// JobHealth — classify a scheduled job from its RUN HISTORY, not its last run.
//
// Why this exists. `daemon jobs diagnose` bucketed jobs by `last_status` and
// `fail_count` read off the jobs doc. Both are point-in-time, and `fail_count`
// is a lifetime tally with no denominator, so the diagnosis could not tell
// these two apart:
//
//   collector crypto universe scorer   fail_count 7687   19389 successes,
//                                                        last one 12m ago
//   Bitcoin hourly event-impact scorer fail_count  180   ZERO successes, ever
//
// They appeared as sibling rows in `current_failures`, and the healthy job had
// the larger fail_count. Meanwhile "Kalshi daily auto cockpit plan" — 1761 runs,
// 0 successes — sat in `historical_failures`, the bucket documented as not
// affecting a healthy execution path. Three separate pipelines rotted for 8
// days to 4 weeks each while the health command reported on them every scan.
//
// The missing fact is the cheapest one available: HAS THIS JOB EVER SUCCEEDED,
// and how many runs ago? `daemon_job_runs` has carried it the whole time
// (indexed on (job_id, started_at DESC)); nothing asked.
//
// Everything here is pure: aggregates in, verdict out. The SQL that computes
// the aggregates lives at the call site so this stays unit-testable.

#include <QString>

namespace openmarketterminal::services::daemon {

// Aggregate of one job's rows in daemon_job_runs.
struct JobRunHistory {
    int total_runs = 0;
    int ok_runs = 0;
    // ISO-8601 UTC of the most recent successful run. EMPTY means the job has
    // never once succeeded -- the distinction the old diagnosis could not draw.
    QString last_success_at;
    // Runs recorded after the last success. Equals total_runs when there has
    // never been a success.
    int runs_since_success = 0;
};

enum class JobHealthClass {
    NoHistory,       // never ran; nothing to report
    Healthy,         // succeeding
    RecentFailure,   // failing now, but has succeeded before and the streak is short
    ChronicFailure,  // has succeeded, but not for a long unbroken streak
    NeverSucceeded,  // enough attempts to be sure, and not one has ever worked
};

// A job gets the benefit of the doubt until it has tried this many times. Below
// this, zero successes is "too early to call", not "broken" -- a newly added
// job must not page anyone on its first failed run.
inline constexpr int kNeverSucceededMinRuns = 3;

// Failing runs since the last success before a job that DOES work sometimes is
// called chronic rather than merely unlucky.
inline constexpr int kChronicFailureStreak = 10;

JobHealthClass classify_job_health(const JobRunHistory& history);

// Stable lowercase token, used as the JSON bucket name and the table cell.
QString job_health_class_label(JobHealthClass klass);

// Does this class mean a human should look? Drives the overall state.
bool job_health_needs_attention(JobHealthClass klass);

// Worst-first ordering key: higher sorts earlier. The whole point of the
// surface is that the never-succeeded job cannot be buried under 49 rows of
// routine historical noise.
int job_health_severity(JobHealthClass klass);

// One-line human summary, e.g.
//   "180 runs, 0 ever succeeded"
//   "1283/5412 ok, last success 20 days ago (4129 runs ago)"
QString job_health_summary(const JobRunHistory& history);

}  // namespace openmarketterminal::services::daemon
