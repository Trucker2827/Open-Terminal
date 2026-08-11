#pragma once

// Which QProcess errors a scheduled job must record for itself.
//
// The daemon's errorOccurred handler discarded its ProcessError argument and
// stamped every one of them "process start error":
//
//     connect(p, &QProcess::errorOccurred, qApp, [..](QProcess::ProcessError) {
//         j["last_error"] = QStringLiteral("process start error");
//
// errorOccurred fires for FailedToStart, Crashed, Timedout, WriteError,
// ReadError and UnknownError. The daemon's own timeout timer calls p->kill(),
// which produces Crashed -- so a job killed at its 45s deadline was filed as a
// failure to SPAWN. "Strategy sandbox tick" carried that label repeatedly while
// every one of those runs had lasted the full ~45s, which is impossible for a
// process that never started.
//
// It also set daemon_history_recorded, suppressing the finished() handler that
// would have written the accurate `timeout` verdict. The wrong label did not
// sit alongside the right one; it replaced it.
//
// Only FailedToStart is terminal WITHOUT a finished() signal, so it is the only
// case the error handler must record. Everything else is followed by
// finished(), which knows the exit code and whether the daemon killed it.

#include <QProcess>
#include <QString>

namespace openmarketterminal::services::daemon {

/// True only for errors after which finished() will NOT arrive -- i.e. the
/// error handler is the last chance to close the run out. Recording anything
/// else here double-counts and overwrites the accurate verdict.
bool job_process_error_is_terminal(QProcess::ProcessError error);

/// Human-readable cause. `detail` is QProcess::errorString(); appended when
/// present so the reader gets the OS-level reason, not just a category.
QString job_process_error_label(QProcess::ProcessError error, const QString& detail = {});

}  // namespace openmarketterminal::services::daemon
