#include "web/HeadlessBrowser.h"

#include <QCoreApplication>

#ifdef HAS_QT_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>

#include <QList>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QPointer>
#include <QSemaphore>
#include <QTimer>
#include <QVariant>

#include <memory>
#include <utility>
#endif

namespace openmarketterminal::web {

HeadlessBrowser& HeadlessBrowser::instance() {
    static HeadlessBrowser inst;
    return inst;
}

HeadlessBrowser::HeadlessBrowser(QObject* parent) : QObject(parent) {
    // QtWebEngine must be driven from the GUI/main thread. Ensure this object's
    // thread affinity is the main thread so QMetaObject::invokeMethod() with a
    // QueuedConnection delivers the load lambda there, regardless of which
    // thread first touched instance().
    if (auto* app = QCoreApplication::instance())
        moveToThread(app->thread());
}

#ifdef HAS_QT_WEBENGINE

namespace {

// A QWebEnginePage that never opens a (blocking) modal JS dialog. QtWebEngine
// exposes dialog handling through these protected virtuals; the default
// implementations would surface UI and block. We make them silent no-ops so a
// headless fetch can never wedge on a dialog.
class SilentWebPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    void javaScriptAlert(const QUrl&, const QString&) override {}
    bool javaScriptConfirm(const QUrl&, const QString&) override { return false; }
    bool javaScriptPrompt(const QUrl&, const QString&, const QString&, QString*) override {
        return false;
    }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString&, int,
                                  const QString&) override {}
};

// Per-call control block. Heap-allocated and shared (shared_ptr) between the
// worker thread (which owns the fetch() stack frame) and the main thread (which
// captures it in the load lambda + timer callbacks). Heap allocation is what
// makes a late main-thread callback safe even if the worker has already given
// up: the callback writes into a live object, never a dangling stack frame.
struct Ctx {
    // --- cross-thread handoff (guarded by the semaphore's release/acquire) ---
    QString result;       // written by main in finish() BEFORE sem.release()
    QSemaphore sem{0};    // worker acquires; main releases exactly once

    // --- main-thread-only state (no locking needed; touched only on GUI thread) ---
    bool mainDone = false;                       // finish() idempotency guard
    QList<QMetaObject::Connection> conns;        // per-call connections to tear down
    QPointer<QTimer> settle;                     // post-load settle timer
    QPointer<QTimer> hard;                       // hard timeout timer
};

}  // namespace

QString HeadlessBrowser::fetch(const QUrl& url, const QString& extraction_js, int timeout_ms) {
    // Defensive: calling fetch() from the GUI thread would deadlock — the
    // QueuedConnection load lambda would never run while this thread is
    // blocked on the semaphore.  All real callers are off-main, so this
    // should never fire; the guard makes the failure mode a clean error
    // instead of a 17-second freeze.
    if (QThread::currentThread() == this->thread()) {
        qWarning("HeadlessBrowser::fetch() must be called off the GUI thread; returning empty");
        return QString();
    }

    // Serialize: only one load at a time on the single shared page_. The main
    // thread never acquires this mutex, so there is no lock-ordering inversion.
    QMutexLocker lock(&serialize_);

    auto ctx = std::make_shared<Ctx>();

    // Drive QtWebEngine on the main/GUI thread. The lambda captures `ctx` by
    // value (shared_ptr), so the main side keeps the control block alive for as
    // long as any of its callbacks can still fire.
    QMetaObject::invokeMethod(
        this,
        [this, url, extraction_js, timeout_ms, ctx]() {
            if (!page_) {
                page_ = new SilentWebPage(QWebEngineProfile::defaultProfile(), this);
                // JavascriptEnabled stays at its default (true): dynamic pages
                // (e.g. DuckDuckGo) need to run JS to populate before we extract.
                page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                                 false);
                page_->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, false);
            }

            ctx->settle = new QTimer(page_);
            ctx->settle->setSingleShot(true);
            ctx->hard = new QTimer(page_);
            ctx->hard->setSingleShot(true);

            // finish(): runs ONLY on the main thread (hard timeout, loadFinished,
            // settle/runJavaScript callback). Idempotent via ctx->mainDone, so no
            // locking is needed for the main-only state. It tears down all per-call
            // connections + timers (critical: the shared page_ outlives the call,
            // so a leftover loadFinished connection would otherwise fire on the
            // NEXT fetch), publishes the result, then releases the worker.
            auto finish = [ctx](const QString& js_result) {
                if (ctx->mainDone)
                    return;
                ctx->mainDone = true;

                if (ctx->settle)
                    ctx->settle->stop();
                if (ctx->hard)
                    ctx->hard->stop();
                for (const auto& c : ctx->conns)
                    QObject::disconnect(c);
                ctx->conns.clear();
                if (ctx->settle)
                    ctx->settle->deleteLater();
                if (ctx->hard)
                    ctx->hard->deleteLater();

                ctx->result = js_result;  // MUST precede release(): establishes
                ctx->sem.release();       // happens-before for the worker's read.
            };

            // Hard timeout on the main side. Fires before the worker-side belt
            // timeout, so in any healthy run the worker is woken at ~timeout_ms.
            ctx->conns << QObject::connect(ctx->hard, &QTimer::timeout, page_,
                                           [finish]() { finish(QString()); });
            ctx->hard->start(timeout_ms);

            ctx->conns << QObject::connect(
                page_, &QWebEnginePage::loadFinished, page_,
                [this, ctx, extraction_js, finish](bool ok) {
                    if (ctx->mainDone)
                        return;  // already finished (e.g. hard timeout)
                    if (!ok) {
                        finish(QString());
                        return;
                    }
                    if (!ctx->settle)
                        return;
                    QWebEnginePage* page = page_;
                    ctx->conns << QObject::connect(
                        ctx->settle, &QTimer::timeout, page, [page, extraction_js, finish]() {
                            page->runJavaScript(extraction_js, [finish](const QVariant& v) {
                                finish(v.toString());
                            });
                        });
                    ctx->settle->start(400);  // let late JS settle before extracting
                });

            page_->setUrl(url);
        },
        Qt::QueuedConnection);

    // Block the worker on the semaphore — no QEventLoop on the worker thread, so
    // we never pump unrelated queued events here. tryAcquire bounds the wait as a
    // belt-and-suspenders backstop (slightly longer than the main-side hard
    // timeout) in case the main thread is wedged and never wakes us.
    if (!ctx->sem.tryAcquire(1, timeout_ms + 2000)) {
        // Timed out worker-side. Do NOT read ctx->result (no happens-before with a
        // possible concurrent main-thread write). The shared ctx keeps any late
        // main-thread callback safe; it simply writes into a live object.
        return QString();
    }
    return ctx->result;  // safe: sem.release() in finish() happened-before this read.
}

#else  // !HAS_QT_WEBENGINE

QString HeadlessBrowser::fetch(const QUrl&, const QString&, int) {
    return QString();  // QtWebEngine unavailable in this build configuration.
}

#endif  // HAS_QT_WEBENGINE

}  // namespace openmarketterminal::web
