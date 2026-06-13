#include "mcp/ToolConfirmationGate.h"

#include "core/logging/Logger.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

namespace openmarketterminal::mcp {

static constexpr auto TAG = "ToolConfirm";

ToolConfirmationGate& ToolConfirmationGate::instance() {
    static ToolConfirmationGate gate;
    return gate;
}

void ToolConfirmationGate::set_presenter(Presenter presenter) {
    QMutexLocker lock(&presenter_mutex_);
    presenter_ = std::move(presenter);
}

bool ToolConfirmationGate::request(const QString& title, const QString& detail) {
    // Serialize: never show two approval prompts at once.
    QMutexLocker serial(&serialize_mutex_);

    Presenter presenter;
    {
        QMutexLocker lock(&presenter_mutex_);
        presenter = presenter_;
    }
    if (!presenter) {
        LOG_WARN(TAG, "Destructive tool DENIED — no confirmation presenter installed (fail-closed).");
        return false;
    }
    if (!qApp) {
        LOG_WARN(TAG, "Destructive tool DENIED — no QApplication to host the prompt (fail-closed).");
        return false;
    }

    // Run the presenter and treat ANY failure as a deny.
    auto run = [&]() -> bool {
        try {
            return presenter(title, detail);
        } catch (...) {
            LOG_WARN(TAG, "Confirmation presenter threw — DENYING (fail-closed).");
            return false;
        }
    };

    // Already on the main thread (e.g. a sync tool call from the UI): show the
    // modal directly. Marshaling to our own thread with a blocking connection
    // would deadlock.
    if (QThread::currentThread() == qApp->thread())
        return run();

    // Worker thread: run the modal on the main thread and block until it answers.
    // BlockingQueuedConnection makes capturing `approved` by reference safe — the
    // worker is suspended until the lambda completes on the main thread.
    bool approved = false;
    const bool invoked = QMetaObject::invokeMethod(
        qApp, [&approved, &run]() { approved = run(); }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        LOG_WARN(TAG, "Destructive tool DENIED — could not marshal prompt to main thread (fail-closed).");
        return false;
    }
    return approved;
}

}  // namespace openmarketterminal::mcp
