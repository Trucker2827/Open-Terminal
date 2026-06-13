// QuantLibLocalServer.cpp — see header.

#include "services/quantlib/QuantLibLocalServer.h"

#include "core/logging/Logger.h"
#include "python/PythonRunner.h"
#include "storage/repositories/SettingsRepository.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace openmarketterminal::services {

static constexpr const char* kTag = "QuantLibLocalServer";

QuantLibLocalServer& QuantLibLocalServer::instance() {
    static QuantLibLocalServer s;
    return s;
}

void QuantLibLocalServer::ensure_running() {
    if (started_) {
        return;
    }
    started_ = true;

    const QString interpreter = openmarketterminal::python::PythonRunner::instance().python_path();
    const QString scripts_dir = openmarketterminal::python::PythonRunner::instance().scripts_dir();
    if (interpreter.isEmpty() || scripts_dir.isEmpty()) {
        LOG_WARN(kTag, "Python interpreter or scripts dir not configured — local QuantLib server not started");
        return;
    }

    const QString script = scripts_dir + "/quantlib/quantlib_server.py";
    if (!QFileInfo::exists(script)) {
        LOG_WARN(kTag, QString("QuantLib server script not found at %1 — not started").arg(script));
        return;
    }

    proc_ = new QProcess();
    proc_->start(interpreter, {script, "--port", "8800"});

    // Never orphan the server: terminate it cleanly when the app quits.
    QProcess* proc = proc_;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, proc, [proc]() {
        proc->terminate();
        proc->waitForFinished(1500);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
        }
    });

    // Point the client + header badge at the local server if not already set.
    auto r = openmarketterminal::SettingsRepository::instance().get(QStringLiteral("connectors.quantlib_url"));
    if (!(r.is_ok() && !r.value().trimmed().isEmpty())) {
        openmarketterminal::SettingsRepository::instance().set(QStringLiteral("connectors.quantlib_url"),
                                                               QStringLiteral("http://127.0.0.1:8800"),
                                                               QStringLiteral("connectors"));
    }

    LOG_INFO(kTag, QString("Local QuantLib server started: %1 %2 --port 8800").arg(interpreter, script));
}

} // namespace openmarketterminal::services
