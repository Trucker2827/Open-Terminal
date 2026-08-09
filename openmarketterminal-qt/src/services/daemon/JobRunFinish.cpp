#include "services/daemon/JobRunFinish.h"

namespace openmarketterminal::services::daemon {

QString job_run_finish_sql() {
    // `AND finished_at IS NULL` is the whole fix: a run's verdict is written
    // once. Without it the reconciler's `stale-timeout` was overwritten by the
    // QProcess finished handler's later `ok`, and the recomputed duration made
    // the row claim a 1398s run had met a 45s timeout.
    return QStringLiteral(
        "UPDATE daemon_job_runs SET "
        "status=:status, exit_code=:exit_code, finished_at=:finished_at, "
        "duration_ms=:duration_ms, stdout_tail=:stdout_tail, "
        "stderr_tail=:stderr_tail, error=:error "
        "WHERE run_id=:run_id AND finished_at IS NULL");
}

}  // namespace openmarketterminal::services::daemon
