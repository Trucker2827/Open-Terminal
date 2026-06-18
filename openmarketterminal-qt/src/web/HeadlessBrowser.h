#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QMutex>

class QWebEnginePage;

namespace openmarketterminal::web {

// Offscreen QtWebEngine page used to fetch/render web pages headlessly.
//
// Threading contract:
//   * The underlying QWebEnginePage lives on the GUI/main thread (where
//     QApplication runs) — QtWebEngine REQUIRES this.
//   * fetch() is BLOCKING and is safe to call from any thread (it is designed
//     to be called from an MCP worker thread). It hops the actual page load to
//     the main thread and blocks the calling thread on a semaphore until the
//     load + extraction completes or a timeout fires.
//   * fetch() is serialized internally: only one load runs at a time on the
//     single shared page.
class HeadlessBrowser : public QObject {
    Q_OBJECT
public:
    static HeadlessBrowser& instance();

    // Loads `url`, waits for load + a short settle, then runs `extraction_js`
    // and returns its string result. Returns an empty QString on load failure
    // or timeout. Blocks the calling thread; callable from any thread.
    QString fetch(const QUrl& url, const QString& extraction_js, int timeout_ms = 15000);

private:
    explicit HeadlessBrowser(QObject* parent = nullptr);

    QWebEnginePage* page_ = nullptr;  // created lazily on the GUI thread
    QMutex serialize_;                // one fetch at a time (shared page)
};

}  // namespace openmarketterminal::web
