#include "python/NotebookKernel.h"

#include "python/PythonRunner.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace openmarketterminal::python {

NotebookKernel& NotebookKernel::instance() {
    static NotebookKernel inst;
    return inst;
}

NotebookKernel::NotebookKernel() {
    // Make sure the kernel process is torn down with the app — no orphans.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { shutdown(); });
}

bool NotebookKernel::ensure_started() {
    if (proc_ && proc_->state() != QProcess::NotRunning)
        return true;

    const QString interpreter = PythonRunner::instance().python_path();
    const QString scripts_dir = PythonRunner::instance().scripts_dir();
    if (interpreter.isEmpty() || scripts_dir.isEmpty())
        return false;

    const QString script = scripts_dir + "/notebook_kernel.py";
    if (!QFileInfo::exists(script))
        return false;

    proc_ = new QProcess(this);
    proc_->setProcessEnvironment(PythonRunner::instance().build_python_env());
    proc_->setProgram(interpreter);
    proc_->setArguments({script});

    connect(proc_, &QProcess::readyReadStandardOutput, this, &NotebookKernel::on_ready_read);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { handle_process_gone(); });
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { handle_process_gone(); });

    proc_->start();  // Qt buffers writes until the process is up.
    return true;
}

void NotebookKernel::send(const QJsonObject& req, Callback cb) {
    if (!ensure_started()) {
        if (cb) {
            Result r;
            r.ok = false;
            r.error = "Python kernel unavailable";
            cb(r);
        }
        return;
    }

    const int id = req.value("id").toInt();
    if (cb)
        callbacks_.insert(id, std::move(cb));

    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    proc_->write(line);
}

void NotebookKernel::execute(const QString& code, Callback cb) {
    const int id = next_id_++;
    QJsonObject req;
    req["id"] = id;
    req["code"] = code;
    send(req, std::move(cb));
}

void NotebookKernel::reset(Callback cb) {
    const int id = next_id_++;
    QJsonObject req;
    req["id"] = id;
    req["cmd"] = "reset";
    send(req, std::move(cb));
}

void NotebookKernel::shutdown() {
    if (!proc_)
        return;
    proc_->disconnect(this);  // don't respawn / fire callbacks during shutdown
    callbacks_.clear();
    buffer_.clear();
    proc_->terminate();
    if (!proc_->waitForFinished(1000)) {
        proc_->kill();
        proc_->waitForFinished(1000);
    }
    proc_->deleteLater();
    proc_ = nullptr;
}

void NotebookKernel::on_ready_read() {
    if (!proc_)
        return;
    buffer_ += proc_->readAllStandardOutput();

    int nl;
    while ((nl = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(nl);
        buffer_.remove(0, nl + 1);
        if (line.trimmed().isEmpty())
            continue;

        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject obj = doc.object();

        const int id = obj.value("id").toInt();
        // Banner (id 0) and any stale/unmatched response have no callback — skip.
        if (!callbacks_.contains(id))
            continue;
        const Callback cb = callbacks_.take(id);
        if (!cb)
            continue;

        Result r;
        r.ok = obj.value("ok").toBool();
        r.stdout_text = obj.value("stdout").toString();
        r.stderr_text = obj.value("stderr").toString();
        r.result_repr = obj.value("result").toString();
        r.error = obj.value("error").toString();
        for (const QJsonValue& v : obj.value("traceback").toArray())
            r.traceback << v.toString();
        cb(r);
    }
}

void NotebookKernel::handle_process_gone() {
    // Fire every pending callback with an honest failure. Copy-then-clear before
    // iterating: a callback may re-enter execute() and mutate callbacks_.
    if (proc_) {
        proc_->disconnect(this);  // finished + errorOccurred can both fire; only handle once
        proc_->deleteLater();     // never delete synchronously inside its own slot
        proc_ = nullptr;
    }
    buffer_.clear();

    const QHash<int, Callback> pending = callbacks_;
    callbacks_.clear();
    for (const Callback& cb : pending) {
        if (cb) {
            Result r;
            r.ok = false;
            r.error = "kernel exited";
            cb(r);
        }
    }
}

} // namespace openmarketterminal::python
