#include "network/websocket/WebSocketClient.h"

#include "core/logging/Logger.h"

#include <QDateTime>
#include <QMetaObject>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

namespace openmarketterminal {

namespace {
constexpr const char* kTag = "WS";

// Mask the `token=` query value so broker access tokens (JWTs) never reach the
// log file. Applied to every URL we log — these tokens are bearer credentials.
QString redact_url(const QString& url) {
    QString out = url;
    static const QRegularExpression re(QStringLiteral("token=[^&]*"));
    out.replace(re, QStringLiteral("token=REDACTED"));
    return out;
}

QString thread_label() {
    auto* t = QThread::currentThread();
    if (!t)
        return QStringLiteral("?");
    const QString name = t->objectName();
    return name.isEmpty() ? QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(t), 0, 16) : name;
}

QNetworkRequest make_request(const QString& url, const QMap<QString, QString>& headers) {
    QNetworkRequest req{QUrl(url)};
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    return req;
}

// Run `fn` on `target`'s own thread. If we're already there, runs immediately;
// otherwise posts via QMetaObject::invokeMethod with QueuedConnection.
//
// Why: QWebSocket / QSslSocket internally bind QSocketNotifier and QTimer to
// the thread that called open()/close()/sendXxx(). Calling those methods from
// another thread corrupts the notifier table — Qt logs "QSocketNotifier:
// Socket notifiers cannot be enabled or disabled from another thread" and then
// the socket eventually crashes with a use-after-free in SSL_CTX_free during
// teardown. Marshalling fixes this for every public WebSocketClient method.
template <typename Fn>
void run_on_owning_thread(QObject* target, Fn&& fn) {
    if (!target)
        return;
    if (QThread::currentThread() == target->thread()) {
        fn();
    } else {
        QMetaObject::invokeMethod(target, std::forward<Fn>(fn), Qt::QueuedConnection);
    }
}
} // namespace

#ifdef HAS_QT_WEBSOCKETS

WebSocketClient::WebSocketClient(QObject* parent) : QObject(parent) {
    // Parent the QWebSocket to `this` so moveToThread on WebSocketClient
    // also relocates the socket (and the QSocketNotifier / QTimer children
    // that QWebSocket creates internally). Otherwise the socket stays on
    // the construction thread while the wrapper has worker-thread affinity,
    // and every connect/close/send call is a cross-thread access — Qt logs
    // "QSocketNotifier: Socket notifiers cannot be enabled or disabled from
    // another thread" and the SSL context eventually use-after-frees on
    // teardown.
    socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(socket_, &QWebSocket::connected, this, &WebSocketClient::on_connected);
    connect(socket_, &QWebSocket::disconnected, this, &WebSocketClient::on_disconnected);
    connect(socket_, &QWebSocket::textMessageReceived, this, &WebSocketClient::on_text_received);
    connect(socket_, &QWebSocket::binaryMessageReceived, this, &WebSocketClient::on_binary_received);
    connect(socket_, &QWebSocket::errorOccurred, this, &WebSocketClient::on_error);
    connect(&reconnect_timer_, &QTimer::timeout, this, &WebSocketClient::attempt_reconnect);
    reconnect_timer_.setSingleShot(true);
}

void WebSocketClient::connect_to(const QString& url) {
    connect_to(url, QMap<QString, QString>{});
}

void WebSocketClient::connect_to(const QString& url, const QMap<QString, QString>& headers) {
    QPointer<WebSocketClient> self(this);
    const QString u = url;
    const QMap<QString, QString> h = headers;
    run_on_owning_thread(this, [self, u, h]() {
        if (!self)
            return;
        self->url_ = u;
        self->headers_ = h;
        self->header_provider_ = nullptr;  // static headers — no per-connect regen
        self->reconnect_attempts_ = 0;
        self->reconnect_stopped_ = false; // a fresh connect re-enables auto-reconnect
        LOG_INFO(kTag, QString("[%1] Connecting to %2%3")
                           .arg(thread_label(), redact_url(u),
                                h.isEmpty() ? QString() : QStringLiteral(" (custom headers)")));
        if (h.isEmpty())
            self->socket_->open(QUrl(u));
        else
            self->socket_->open(make_request(u, h));
    });
}

void WebSocketClient::connect_to(const QString& url, HeaderProvider provider) {
    QPointer<WebSocketClient> self(this);
    const QString u = url;
    auto p = std::move(provider);
    run_on_owning_thread(this, [self, u, p]() {
        if (!self)
            return;
        self->url_ = u;
        self->header_provider_ = p;
        self->headers_.clear();
        self->reconnect_attempts_ = 0;
        self->reconnect_stopped_ = false;
        // Generate fresh headers for this initial connect.
        const QMap<QString, QString> h = p ? p() : QMap<QString, QString>{};
        LOG_INFO(kTag, QString("[%1] Connecting to %2 (auth headers)")
                           .arg(thread_label(), redact_url(u)));
        if (h.isEmpty())
            self->socket_->open(QUrl(u));
        else
            self->socket_->open(make_request(u, h));
    });
}

void WebSocketClient::disconnect() {
    QPointer<WebSocketClient> self(this);
    run_on_owning_thread(this, [self]() {
        if (!self)
            return;
        LOG_INFO(kTag, QString("[%1] Disconnect requested for %2").arg(thread_label(), redact_url(self->url_)));
        self->reconnect_timer_.stop();
        self->socket_->close();
    });
}

void WebSocketClient::stop_reconnect() {
    QPointer<WebSocketClient> self(this);
    run_on_owning_thread(this, [self]() {
        if (!self)
            return;
        self->reconnect_stopped_ = true;
        self->reconnect_timer_.stop();
        LOG_INFO(kTag, QString("[%1] Auto-reconnect halted for %2 (fatal error or explicit stop)")
                           .arg(thread_label(), redact_url(self->url_)));
    });
}

void WebSocketClient::send(const QString& message) {
    QPointer<WebSocketClient> self(this);
    const QString m = message;
    run_on_owning_thread(this, [self, m]() {
        if (!self)
            return;
        self->socket_->sendTextMessage(m);
    });
}

void WebSocketClient::send_binary(const QByteArray& data) {
    QPointer<WebSocketClient> self(this);
    const QByteArray d = data;
    run_on_owning_thread(this, [self, d]() {
        if (!self)
            return;
        self->socket_->sendBinaryMessage(d);
    });
}

bool WebSocketClient::is_connected() const {
    // Read of QAbstractSocket::state is documented as thread-safe.
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState;
}

void WebSocketClient::on_connected() {
    LOG_INFO(kTag, QString("[%1] Connected to %2").arg(thread_label(), url_));
    reconnect_attempts_ = 0;
    emit connected();
}

void WebSocketClient::on_disconnected() {
    LOG_WARN(kTag, QString("[%1] Disconnected from %2 (state=%3)")
                       .arg(thread_label(), url_)
                       .arg(static_cast<int>(socket_ ? socket_->state() : QAbstractSocket::UnconnectedState)));
    emit disconnected();
    if (!reconnect_stopped_ && reconnect_attempts_ < MAX_RECONNECT_ATTEMPTS) {
        const int delay = std::min(1000 * (1 << reconnect_attempts_), 30000);
        LOG_INFO(kTag, QString("[%1] Scheduling reconnect attempt %2/%3 in %4ms")
                           .arg(thread_label())
                           .arg(reconnect_attempts_ + 1)
                           .arg(MAX_RECONNECT_ATTEMPTS)
                           .arg(delay));
        reconnect_timer_.start(delay);
    } else {
        LOG_ERROR(kTag, QString("[%1] Max reconnect attempts (%2) reached for %3")
                            .arg(thread_label())
                            .arg(MAX_RECONNECT_ATTEMPTS)
                            .arg(url_));
    }
}

void WebSocketClient::on_text_received(const QString& msg) {
    emit message_received(msg);
}

void WebSocketClient::on_binary_received(const QByteArray& data) {
    emit binary_message_received(data);
}

void WebSocketClient::on_error(QAbstractSocket::SocketError err) {
    const QString es = socket_ ? socket_->errorString() : QStringLiteral("(no socket)");
    LOG_ERROR(kTag, QString("[%1] Error on %2: %3 (code=%4)")
                       .arg(thread_label(), redact_url(url_), es)
                       .arg(static_cast<int>(err)));
    emit error_occurred(es);
}

void WebSocketClient::attempt_reconnect() {
    if (reconnect_stopped_) {
        LOG_INFO(kTag, QString("[%1] attempt_reconnect skipped for %2 (stop_reconnect)")
                           .arg(thread_label(), redact_url(url_)));
        return;
    }
    reconnect_attempts_++;
    LOG_INFO(kTag, QString("[%1] Reconnect attempt %2/%3 to %4")
                       .arg(thread_label())
                       .arg(reconnect_attempts_)
                       .arg(MAX_RECONNECT_ATTEMPTS)
                       .arg(redact_url(url_)));
    // Regenerate auth headers per attempt for time-sensitive auth (Kalshi); a
    // replayed stale signature/timestamp is rejected with HTTP 401.
    const QMap<QString, QString> h = header_provider_ ? header_provider_() : headers_;
    if (h.isEmpty())
        socket_->open(QUrl(url_));
    else
        socket_->open(make_request(url_, h));
}

#else // No Qt WebSockets — stub implementations

WebSocketClient::WebSocketClient(QObject* parent) : QObject(parent) {}
void WebSocketClient::connect_to(const QString& /*url*/) {
    LOG_WARN(kTag, "WebSocket not available — Qt6::WebSockets not installed");
}
void WebSocketClient::connect_to(const QString& /*url*/, const QMap<QString, QString>& /*headers*/) {
    LOG_WARN(kTag, "WebSocket not available — Qt6::WebSockets not installed");
}
void WebSocketClient::connect_to(const QString& /*url*/, HeaderProvider /*provider*/) {
    LOG_WARN(kTag, "WebSocket not available — Qt6::WebSockets not installed");
}
void WebSocketClient::disconnect() {}
void WebSocketClient::stop_reconnect() {}
void WebSocketClient::send(const QString& /*message*/) {}
void WebSocketClient::send_binary(const QByteArray& /*data*/) {}
bool WebSocketClient::is_connected() const {
    return false;
}

#endif

} // namespace openmarketterminal
