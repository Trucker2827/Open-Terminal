#pragma once

// The UPDATE that finalises a daemon job-run history row.
//
// Why this is its own unit. A run can be finalised TWICE by two independent
// paths that do not coordinate:
//
//   1. reconcile_stale_running_jobs() declares a run `stale-timeout` once it
//      has outlived timeout_sec + 15s.
//   2. the QProcess `finished` handler later reports the real exit, `ok`.
//
// The QProcess error/finished pair already guard each other with the
// `daemon_history_recorded` property. The reconciler has no such handshake, so
// the second write silently overwrote the first -- and because the UPDATE
// recomputes duration_ms from the row's ORIGINAL started_at, the result was a
// row reading `ok` with a duration of many minutes against a 45s timeout.
//
// The damage is to the record, not the scheduler: on this machine only 36
// `stale-timeout` rows survived since Aug 1 while 1739+ runs had outlived
// their timeout. Anything reading that history -- the health surface in
// particular -- was being told a job met its deadline when it had blown it.
//
// First finalisation wins. A run's verdict is decided once.

#include <QString>

namespace openmarketterminal::services::daemon {

// Guarded UPDATE: only finalises a row that is still open. Bind :status,
// :exit_code, :finished_at, :duration_ms, :stdout_tail, :stderr_tail, :error
// and :run_id. Check numRowsAffected() to learn whether this write won.
QString job_run_finish_sql();

}  // namespace openmarketterminal::services::daemon
