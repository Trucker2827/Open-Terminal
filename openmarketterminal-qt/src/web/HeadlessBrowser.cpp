#include "web/HeadlessBrowser.h"

#include <QCoreApplication>

#ifdef HAS_QT_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineHttpRequest>

#include <QList>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QPointer>
#include <QSemaphore>
#include <QTimer>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>

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

// Overlay-stripping extraction JS for article fetches.
// Removes tracking scripts/styles/chrome, paywall overlays, fixed/sticky
// blocking elements, un-blurs body, restores overflow, then returns
// {title, text} as JSON.
const QString kReaderExtractionJs = QStringLiteral(
    "(function(){"
    "var t=document.title;"
    // Remove scripts, styles, structural chrome
    "document.querySelectorAll('script,style,nav,header,footer,noscript,aside,iframe').forEach(function(e){e.remove();});"
    // Remove paywall/overlay/modal elements by class or id keywords
    "var pwRe=/paywall|subscribe|gate|metered|piano|regwall|modal|overlay|backdrop/i;"
    "document.querySelectorAll('*').forEach(function(e){"
    "  var id=e.id||'';"
    "  var cls=e.className&&typeof e.className==='string'?e.className:'';"
    "  if(pwRe.test(id)||pwRe.test(cls)){e.remove();return;}"
    "  try{"
    "    var cs=window.getComputedStyle(e);"
    "    var pos=cs.getPropertyValue('position');"
    "    if(pos==='fixed'||pos==='sticky'){e.remove();}"
    "  }catch(ex){}"
    "});"
    // Restore readability: un-blur, un-clip, un-hide body/html
    "try{"
    "  document.body.style.overflow='auto';"
    "  document.body.style.position='static';"
    "  document.body.style.maxHeight='';"
    "  document.body.style.filter='';"
    "  document.body.style.webkitFilter='';"
    "  document.documentElement.style.overflow='auto';"
    "  var main=document.querySelector('main,article,[role=\"main\"]');"
    "  if(main){"
    "    main.style.filter='';"
    "    main.style.webkitFilter='';"
    "    main.style.maxHeight='';"
    "    main.style.overflow='auto';"
    "  }"
    "}catch(ex){}"
    "var body=document.body?document.body.innerText:'';"
    "return JSON.stringify({title:t,text:body});"
    "})()");

}  // namespace

// ── Shared implementation ─────────────────────────────────────────────────────

// fetch_on_page: drives the given page on the GUI thread, blocks the caller
// until done or timeout.  Must be called with serialize_ held.
QString HeadlessBrowser::fetch_on_page(QWebEnginePage* page_to_use, const QUrl& url,
                                        const QString& extraction_js, int timeout_ms,
                                        const QString& referer) {
    auto ctx = std::make_shared<Ctx>();

    QMetaObject::invokeMethod(
        this,
        [page_to_use, url, extraction_js, timeout_ms, referer, ctx]() {
            // page_to_use was selected by the caller (page_ or reader_page_).
            // All references inside this lambda use this local alias — never
            // the member `page_` directly — so the reader path stays isolated.
            QWebEnginePage* page = page_to_use;

            ctx->settle = new QTimer(page);
            ctx->settle->setSingleShot(true);
            ctx->hard = new QTimer(page);
            ctx->hard->setSingleShot(true);

            // finish(): runs ONLY on the main thread. Idempotent via ctx->mainDone.
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

                ctx->result = js_result;  // MUST precede release()
                ctx->sem.release();
            };

            ctx->conns << QObject::connect(ctx->hard, &QTimer::timeout, page,
                                           [finish]() { finish(QString()); });
            ctx->hard->start(timeout_ms);

            ctx->conns << QObject::connect(
                page, &QWebEnginePage::loadFinished, page,
                [page, ctx, extraction_js, finish](bool ok) {
                    if (ctx->mainDone)
                        return;
                    if (!ok) {
                        finish(QString());
                        return;
                    }
                    if (!ctx->settle)
                        return;
                    ctx->conns << QObject::connect(
                        ctx->settle, &QTimer::timeout, page, [page, extraction_js, finish]() {
                            page->runJavaScript(extraction_js, [finish](const QVariant& v) {
                                finish(v.toString());
                            });
                        });
                    ctx->settle->start(400);
                });

            if (referer.isEmpty()) {
                page->setUrl(url);
            } else {
                QWebEngineHttpRequest req(url);
                req.setHeader(QByteArrayLiteral("Referer"), referer.toUtf8());
                page->load(req);
            }
        },
        Qt::QueuedConnection);

    if (!ctx->sem.tryAcquire(1, timeout_ms + 2000))
        return QString();
    return ctx->result;
}

// ── Public fetch overloads ────────────────────────────────────────────────────

QString HeadlessBrowser::fetch(const QUrl& url, const QString& extraction_js, int timeout_ms) {
    if (QThread::currentThread() == this->thread()) {
        qWarning("HeadlessBrowser::fetch() must be called off the GUI thread; returning empty");
        return QString();
    }

    QMutexLocker lock(&serialize_);

    // Lazy-create the default page on the GUI thread.
    if (!page_) {
        QMetaObject::invokeMethod(this, [this]() {
            if (!page_) {
                page_ = new SilentWebPage(QWebEngineProfile::defaultProfile(), this);
                page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
                page_->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, false);
            }
        }, Qt::BlockingQueuedConnection);
    }

    return fetch_on_page(page_, url, extraction_js, timeout_ms, {});
}

QString HeadlessBrowser::fetch(const QUrl& url, const QString& extraction_js, int timeout_ms,
                                bool reader_mode, const QString& referer) {
    if (!reader_mode)
        return fetch(url, extraction_js, timeout_ms);

    if (QThread::currentThread() == this->thread()) {
        qWarning("HeadlessBrowser::fetch() must be called off the GUI thread; returning empty");
        return QString();
    }

    QMutexLocker lock(&serialize_);

    // Lazy-create the off-the-record reader profile + page on the GUI thread.
    if (!reader_page_) {
        QMetaObject::invokeMethod(this, [this]() {
            if (!reader_page_) {
                // new QWebEngineProfile(this) — no name → off-the-record (no persistent cookies)
                reader_profile_ = new QWebEngineProfile(this);
                reader_profile_->setHttpUserAgent(
                    QStringLiteral("Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)"));
                reader_page_ = new SilentWebPage(reader_profile_, this);
                reader_page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
                reader_page_->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, false);
            }
        }, Qt::BlockingQueuedConnection);
    }

    const QString ref = referer.isEmpty() ? QStringLiteral("https://www.google.com/") : referer;
    return fetch_on_page(reader_page_, url, extraction_js, timeout_ms, ref);
}

// ── Multi-strategy article fetch ──────────────────────────────────────────────

/*static*/
QString HeadlessBrowser::fetch_article_best(const QString& url) {
    auto parse_text = [](const QString& raw) -> QString {
        if (raw.isEmpty())
            return {};
        QJsonParseError err;
        auto doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object().value(QStringLiteral("text")).toString();
        return {};
    };

    HeadlessBrowser& hb = instance();

    // Strategy A: plain fetch with the normal browser UA. This is the proven
    // path that works for the vast majority of sites — it MUST be tried first.
    // (Leading with the Googlebot UA regressed every normal site that gates
    // crawler user-agents into the "couldn't load" note.)
    QString best = parse_text(hb.fetch(QUrl(url), kReaderExtractionJs, 20000));

    // Strategy B: Googlebot reader-mode (cookieless + crawler UA + Google
    // referer) — only when the plain fetch came up thin (metered/soft paywall).
    if (best.length() < 600) {
        QString text_b = parse_text(hb.fetch(QUrl(url), kReaderExtractionJs, 20000,
                                             /*reader_mode=*/true,
                                             QStringLiteral("https://www.google.com/")));
        if (text_b.length() > best.length())
            best = text_b;
    }

    // Strategy C: archive.today snapshot — last resort when both direct fetches
    // are thin (hard paywall with an existing snapshot).
    if (best.length() < 600) {
        const QString archive_url = QStringLiteral("https://archive.ph/newest/") + url;
        QString text_c = parse_text(hb.fetch(QUrl(archive_url), kReaderExtractionJs, 25000,
                                             /*reader_mode=*/true,
                                             QStringLiteral("https://www.google.com/")));
        if (text_c.length() > best.length())
            best = text_c;
    }

    return best;
}

#else  // !HAS_QT_WEBENGINE

QString HeadlessBrowser::fetch(const QUrl&, const QString&, int) {
    return QString();  // QtWebEngine unavailable in this build configuration.
}

QString HeadlessBrowser::fetch(const QUrl&, const QString&, int, bool, const QString&) {
    return QString();
}

/*static*/
QString HeadlessBrowser::fetch_article_best(const QString&) {
    return QString();
}

QString HeadlessBrowser::fetch_on_page(QWebEnginePage*, const QUrl&, const QString&, int,
                                        const QString&) {
    return QString();
}

#endif  // HAS_QT_WEBENGINE

}  // namespace openmarketterminal::web
