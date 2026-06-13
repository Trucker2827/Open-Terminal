#pragma once
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace openmarketterminal::python {

/// Owns a single long-running Python "kernel" process (scripts/notebook_kernel.py)
/// so notebook cells share a namespace — a real kernel, not a fresh subprocess per
/// cell. One JSON object per line over stdin/stdout; responses matched by id.
class NotebookKernel : public QObject {
    Q_OBJECT
  public:
    struct Result {
        bool ok = false;
        QString stdout_text;
        QString stderr_text;
        QString result_repr;
        QString error;
        QStringList traceback;
    };
    using Callback = std::function<void(Result)>;

    static NotebookKernel& instance();

    /// Run a cell. Lazily starts the kernel; the callback fires when the matching
    /// response arrives (or immediately with ok=false if the kernel is unavailable).
    void execute(const QString& code, Callback cb);

    /// Clear the kernel namespace (restart state). cb fires on the reset response.
    void reset(Callback cb = {});

  private:
    NotebookKernel();

    bool ensure_started();        // returns true if the process is running/started
    void on_ready_read();         // parse complete lines from stdout
    void handle_process_gone();   // fire pending callbacks with "kernel exited"
    void send(const QJsonObject& req, Callback cb);

    QProcess* proc_ = nullptr;
    QByteArray buffer_;
    QHash<int, Callback> callbacks_;
    int next_id_ = 1;
};

} // namespace openmarketterminal::python
