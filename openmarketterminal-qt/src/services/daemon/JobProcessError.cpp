#include "services/daemon/JobProcessError.h"

namespace openmarketterminal::services::daemon {

bool job_process_error_is_terminal(QProcess::ProcessError error) {
    // Qt emits finished() after Crashed, Timedout, WriteError, ReadError and
    // UnknownError. FailedToStart is the sole exception.
    return error == QProcess::FailedToStart;
}

QString job_process_error_label(QProcess::ProcessError error, const QString& detail) {
    QString base;
    switch (error) {
        case QProcess::FailedToStart:
            base = QStringLiteral("process failed to start");
            break;
        case QProcess::Crashed:
            // Includes the daemon's own timeout kill; finished() distinguishes
            // those and records `timeout`.
            base = QStringLiteral("process terminated");
            break;
        case QProcess::Timedout:
            base = QStringLiteral("process wait timed out");
            break;
        case QProcess::WriteError:
            base = QStringLiteral("process write error");
            break;
        case QProcess::ReadError:
            base = QStringLiteral("process read error");
            break;
        default:
            base = QStringLiteral("process error");
            break;
    }
    const QString trimmed = detail.trimmed();
    return trimmed.isEmpty() ? base : base + QStringLiteral(": ") + trimmed;
}

}  // namespace openmarketterminal::services::daemon
