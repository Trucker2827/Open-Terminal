#include "services/prediction/kalshi/KalshiWsClient.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "datahub/TopicPolicy.h"
#include "network/websocket/WebSocketClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace pr = openmarketterminal::services::prediction;

static constexpr int kPingIntervalMs = 20000;

static double kalshi_fp_to_double(const QJsonValue& v) {
    if (v.isString()) return v.toString().toDouble();
    return v.toDouble();
}

struct EvpKeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct BioDeleter {
    void operator()(BIO* bio) const { BIO_free(bio); }
};

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

using EvpKeyPtr = std::unique_ptr<EVP_PKEY, EvpKeyDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

static QString sign_rsa_pss_sha256(const QString& pem, const QString& message, QString* error) {
    const QByteArray pem_bytes = pem.toUtf8();
    BioPtr bio(BIO_new_mem_buf(pem_bytes.constData(), pem_bytes.size()));
    if (!bio) {
        if (error) *error = QStringLiteral("Could not allocate PEM reader");
        return {};
    }

    EvpKeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!key) {
        if (error) *error = QStringLiteral("Could not parse Kalshi RSA private key");
        return {};
    }

    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        if (error) *error = QStringLiteral("Could not allocate signing context");
        return {};
    }

    EVP_PKEY_CTX* pctx = nullptr;
    if (EVP_DigestSignInit(ctx.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1 || !pctx) {
        if (error) *error = QStringLiteral("Could not initialize RSA-PSS signing");
        return {};
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
        if (error) *error = QStringLiteral("Could not configure RSA-PSS signing");
        return {};
    }

    const QByteArray msg = message.toUtf8();
    if (EVP_DigestSignUpdate(ctx.get(), msg.constData(), msg.size()) != 1) {
        if (error) *error = QStringLiteral("Could not feed signing payload");
        return {};
    }

    size_t sig_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1 || sig_len == 0) {
        if (error) *error = QStringLiteral("Could not measure signature length");
        return {};
    }
    QByteArray sig;
    sig.resize(static_cast<qsizetype>(sig_len));
    if (EVP_DigestSignFinal(ctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &sig_len) != 1) {
        if (error) *error = QStringLiteral("Could not create signature");
        return {};
    }
    sig.resize(static_cast<qsizetype>(sig_len));
    return QString::fromLatin1(sig.toBase64());
}

static QMap<QString, QString> kalshi_ws_auth_headers(const KalshiCredentials& creds, QString* error) {
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString signing_path = QStringLiteral("/trade-api/ws/v2");
    const QString sig = sign_rsa_pss_sha256(creds.private_key_pem,
                                            ts + QStringLiteral("GET") + signing_path,
                                            error);
    if (sig.isEmpty()) return {};
    return {
        {QStringLiteral("KALSHI-ACCESS-KEY"), creds.api_key_id},
        {QStringLiteral("KALSHI-ACCESS-SIGNATURE"), sig},
        {QStringLiteral("KALSHI-ACCESS-TIMESTAMP"), ts},
    };
}

// ── Construction / lifecycle ────────────────────────────────────────────────

KalshiWsClient::KalshiWsClient(QObject* parent) : QObject(parent) {
    ws_ = new openmarketterminal::WebSocketClient(this);
    connect(ws_, &openmarketterminal::WebSocketClient::connected, this, &KalshiWsClient::on_connected);
    connect(ws_, &openmarketterminal::WebSocketClient::disconnected, this, &KalshiWsClient::on_disconnected);
    connect(ws_, &openmarketterminal::WebSocketClient::message_received, this, &KalshiWsClient::on_message);
    connect(ws_, &openmarketterminal::WebSocketClient::error_occurred, this, &KalshiWsClient::on_error);

    ping_timer_ = new QTimer(this);
    ping_timer_->setInterval(kPingIntervalMs);
    connect(ping_timer_, &QTimer::timeout, this, &KalshiWsClient::send_ping);
    book_publish_timer_ = new QTimer(this);
    book_publish_timer_->setSingleShot(true);
    book_publish_timer_->setInterval(25);
    connect(book_publish_timer_, &QTimer::timeout, this, [this]() {
        const auto pending = pending_book_publishes_;
        pending_book_publishes_.clear();
        for (auto it = pending.cbegin(); it != pending.cend(); ++it)
            publish_books(it.key(), it.value());
    });
}

KalshiWsClient::~KalshiWsClient() = default;

// ── Credentials ─────────────────────────────────────────────────────────────

void KalshiWsClient::set_credentials(const KalshiCredentials& creds) {
    const bool had = creds_.is_valid();
    creds_ = creds;
    if (had && !creds_.is_valid()) {
        disconnect();
    }
    if (creds_.is_valid() && (!subscribed_tickers_.isEmpty() || !cf_indices_.isEmpty())) {
        ensure_connected();
    }
}

void KalshiWsClient::subscribe_cf_indices(const QStringList& index_ids) {
    for (const auto& id : index_ids) {
        const QString normalized = id.trimmed().toUpper();
        if (!normalized.isEmpty()) cf_indices_.insert(normalized);
    }
    if (!creds_.is_valid() || cf_indices_.isEmpty()) return;
    ensure_connected();
    if (connected_) send_cf_subscribe();
}

bool KalshiWsClient::has_credentials() const { return creds_.is_valid(); }

// ── Subscribe ───────────────────────────────────────────────────────────────

void KalshiWsClient::subscribe(const QStringList& market_tickers) {
    for (const auto& t : market_tickers) subscribed_tickers_.insert(t);
    if (!creds_.is_valid()) {
        LOG_INFO("KalshiWS",
                 "Subscribe deferred — no credentials configured (Phase 7 will enable streaming)");
        return;
    }
    ensure_connected();
    if (connected_) send_subscribe(market_tickers);
}

void KalshiWsClient::unsubscribe(const QStringList& market_tickers) {
    for (const auto& t : market_tickers) subscribed_tickers_.remove(t);
    if (subscribed_tickers_.isEmpty()) disconnect();
}

void KalshiWsClient::unsubscribe_all() {
    subscribed_tickers_.clear();
    disconnect();
}

void KalshiWsClient::disconnect() {
    ping_timer_->stop();
    ws_->disconnect();
}

void KalshiWsClient::restart() {
    if (!creds_.is_valid() || (subscribed_tickers_.isEmpty() && cf_indices_.isEmpty())) return;
    restart_requested_ = true;
    if (ws_->is_connected()) {
        disconnect();
        return;
    }
    restart_requested_ = false;
    ensure_connected();
}

void KalshiWsClient::ensure_connected() {
    if (ws_->is_connected()) return;
    if (!creds_.is_valid()) return;

    // Validate up front that we can actually sign with the current key.
    QString err;
    if (kalshi_ws_auth_headers(creds_, &err).isEmpty()) {
        LOG_ERROR("KalshiWS", QStringLiteral("WebSocket auth failed: ") + err);
        emit connection_status_changed(false);
        return;
    }
    const QString url = creds_.use_demo ? QString::fromLatin1(kDemoWs) : QString::fromLatin1(kProdWs);
    LOG_INFO("KalshiWS",
             QStringLiteral("Connecting to ") + (creds_.use_demo ? QStringLiteral("demo")
                                                                 : QStringLiteral("production")) +
                 QStringLiteral(" WebSocket"));
    // Pass a provider, not a fixed header set: Kalshi signs the current
    // timestamp, which goes stale within seconds. Auto-reconnect must re-sign
    // each attempt or the replayed signature is rejected with HTTP 401
    // ("Unsupported WWW-Authenticate challenges" from Qt's handshake parser).
    const KalshiCredentials creds = creds_;
    ws_->connect_to(url, [creds]() {
        QString e;
        return kalshi_ws_auth_headers(creds, &e);
    });
}

void KalshiWsClient::send_subscribe(const QStringList& tickers) {
    if (!connected_ || !ws_ || !ws_->is_connected()) return;
    QJsonObject params;
    QJsonArray channels;
    // Kalshi v2 (Apr 2026):
    //   - `ticker` is the current L1 channel (yes_bid_dollars, *_size_fp).
    //   - `orderbook_delta` + `orderbook_snapshot` for L2 book state.
    //   - `trade` streams the public trade tape.
    //   - `market_lifecycle_v2` notifies when markets open/close/settle
    //     (the v1 `market_lifecycle` channel has been superseded).
    channels.append("orderbook_delta");
    channels.append("ticker");
    channels.append("trade");
    channels.append("market_lifecycle_v2");
    params.insert("channels", channels);
    QJsonArray tarr;
    for (const auto& t : tickers) tarr.append(t);
    params.insert("market_tickers", tarr);
    // Kalshi is migrating order-book streams to a unified YES-price scale.
    // Request it explicitly, then convert NO levels back to native NO prices
    // at the parser boundary so the rest of OpenTerminal remains unchanged.
    params.insert("use_yes_price", true);

    QJsonObject msg;
    msg.insert("id", next_msg_id_++);
    msg.insert("cmd", "subscribe");
    msg.insert("params", params);

    ws_->send(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    LOG_INFO("KalshiWS", "Subscribed to " + QString::number(tickers.size()) + " tickers");
}

void KalshiWsClient::request_orderbook_snapshot(const QString& ticker) {
    request_orderbook_snapshots({ticker});
}

void KalshiWsClient::request_orderbook_snapshots(const QStringList& tickers) {
    if (!connected_ || !ws_ || !ws_->is_connected() || tickers.isEmpty() ||
        orderbook_subscription_sid_ <= 0) return;
    QJsonArray market_tickers;
    for (const QString& ticker : tickers) {
        const QString normalized = ticker.trimmed().toUpper();
        if (!normalized.isEmpty() && subscribed_tickers_.contains(normalized))
            market_tickers.append(normalized);
    }
    if (market_tickers.isEmpty()) return;
    QJsonObject params{{QStringLiteral("sids"), QJsonArray{orderbook_subscription_sid_}},
                       {QStringLiteral("action"), QStringLiteral("get_snapshot")},
                       {QStringLiteral("market_tickers"), market_tickers}};
    QJsonObject msg{{QStringLiteral("id"), next_msg_id_++},
                    {QStringLiteral("cmd"), QStringLiteral("update_subscription")},
                    {QStringLiteral("params"), params}};
    ws_->send(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void KalshiWsClient::send_account_subscribe() {
    if (!connected_ || !ws_ || !ws_->is_connected()) return;
    QJsonObject params;
    params.insert(QStringLiteral("channels"),
                  QJsonArray{QStringLiteral("fill"), QStringLiteral("user_orders"),
                             QStringLiteral("market_positions")});
    QJsonObject msg{{QStringLiteral("id"), next_msg_id_++},
                    {QStringLiteral("cmd"), QStringLiteral("subscribe")},
                    {QStringLiteral("params"), params}};
    ws_->send(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void KalshiWsClient::send_cf_subscribe() {
    if (!connected_ || !ws_ || !ws_->is_connected() || cf_indices_.isEmpty()) return;
    QJsonArray ids;
    for (const auto& id : cf_indices_) ids.append(id);
    QJsonObject params{{QStringLiteral("channels"), QJsonArray{QStringLiteral("cfbenchmarks_value")}},
                       {QStringLiteral("index_ids"), ids}};
    QJsonObject msg{{QStringLiteral("id"), next_msg_id_++},
                    {QStringLiteral("cmd"), QStringLiteral("subscribe")},
                    {QStringLiteral("params"), params}};
    ws_->send(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void KalshiWsClient::publish_books(const QString& ticker, qint64 ts_ms) {
    const auto it = books_.constFind(ticker);
    if (it == books_.cend() || !it->has_snapshot) return;

    auto make_book = [ts_ms](const QString& asset_id, const QMap<int, double>& own,
                             const QMap<int, double>& opposite) {
        pr::PredictionOrderBook book;
        book.asset_id = asset_id;
        book.last_update_ms = ts_ms;
        for (auto level = own.constEnd(); level != own.constBegin();) {
            --level;
            if (level.value() > 0.0)
                book.bids.push_back({level.key() / 10000.0, level.value()});
        }
        for (auto level = opposite.constEnd(); level != opposite.constBegin();) {
            --level;
            if (level.value() > 0.0)
                book.asks.push_back({1.0 - level.key() / 10000.0, level.value()});
        }
        return book;
    };

    publish_orderbook(ticker + QStringLiteral(":yes"),
                      make_book(ticker + QStringLiteral(":yes"), it->yes_bids, it->no_bids));
    publish_orderbook(ticker + QStringLiteral(":no"),
                      make_book(ticker + QStringLiteral(":no"), it->no_bids, it->yes_bids));
}

void KalshiWsClient::schedule_book_publish(const QString& ticker, qint64 ts_ms) {
    pending_book_publishes_.insert(ticker, ts_ms);
    if (!book_publish_timer_->isActive()) book_publish_timer_->start();
}

void KalshiWsClient::send_ping() {
    if (!connected_ || !ws_ || !ws_->is_connected()) return;
    QJsonObject ping;
    ping.insert("id", next_msg_id_++);
    ping.insert("cmd", "ping");
    ws_->send(QString::fromUtf8(QJsonDocument(ping).toJson(QJsonDocument::Compact)));
}

// ── Socket handlers ─────────────────────────────────────────────────────────

void KalshiWsClient::on_connected() {
    connected_ = true;
    books_.clear();
    orderbook_sequence_ = 0;
    orderbook_subscription_sid_ = 0;
    ping_timer_->start();
    LOG_INFO("KalshiWS", "Connected");
    emit liveness_activity(QDateTime::currentMSecsSinceEpoch());
    emit connection_status_changed(true);
    if (!subscribed_tickers_.isEmpty()) {
        send_subscribe(QStringList(subscribed_tickers_.begin(), subscribed_tickers_.end()));
    }
    send_account_subscribe();
    send_cf_subscribe();
}

void KalshiWsClient::on_disconnected() {
    connected_ = false;
    ping_timer_->stop();
    LOG_WARN("KalshiWS", "Disconnected");
    emit connection_status_changed(false);
    if (restart_requested_) {
        restart_requested_ = false;
        QTimer::singleShot(0, this, [this]() { ensure_connected(); });
    }
}

void KalshiWsClient::on_error(const QString& err) {
    LOG_ERROR("KalshiWS", "Error: " + err);
}

void KalshiWsClient::on_message(const QString& msg) {
    if (msg.isEmpty()) return;
    QJsonParseError perr;
    auto doc = QJsonDocument::fromJson(msg.toUtf8(), &perr);
    if (doc.isNull() || !doc.isObject()) return;
    const auto obj = doc.object();
    const QString type = obj.value("type").toString();
    // Any valid inbound protocol frame proves that the socket is alive. Pong
    // is deliberately handled before market payload parsing: quiet books must
    // not be mistaken for a dead transport.
    emit liveness_activity(QDateTime::currentMSecsSinceEpoch());
    if (type == QStringLiteral("pong")) return;
    const auto payload = obj.value("msg").toObject();
    const QString ticker = payload.value("market_ticker").toString();

    if (type == QStringLiteral("subscribed")) {
        if (payload.value("channel").toString() == QStringLiteral("orderbook_delta"))
            orderbook_subscription_sid_ = payload.value("sid").toInt();
        return;
    }

    if (type == QStringLiteral("ticker")) {
        if (ticker.isEmpty()) return;
        emit ticker_event(ticker, payload);
        const double yes_price = kalshi_fp_to_double(payload.value("yes_bid_dollars"));
        if (yes_price > 0) publish_price(ticker + QStringLiteral(":yes"), yes_price);
        double no_price = kalshi_fp_to_double(payload.value("no_bid_dollars"));
        if (no_price <= 0.0) {
            const double yes_ask = kalshi_fp_to_double(payload.value("yes_ask_dollars"));
            if (yes_ask > 0.0) no_price = 1.0 - yes_ask;
        }
        if (no_price > 0) publish_price(ticker + QStringLiteral(":no"), no_price);
        return;
    }

    if (type == QStringLiteral("trade")) {
        if (ticker.isEmpty()) return;
        emit trade_event(ticker, payload);
        pr::PredictionTrade t;
        t.asset_id = ticker + QStringLiteral(":yes");
        QString ts_side = payload.value("taker_outcome_side").toString().toLower();
        if (ts_side.isEmpty()) ts_side = payload.value("taker_side").toString().toLower();
        t.side = (ts_side == QStringLiteral("no")) ? QStringLiteral("SELL")
                                                   : QStringLiteral("BUY");
        t.price = kalshi_fp_to_double(payload.value("yes_price_dollars"));
        t.size = kalshi_fp_to_double(payload.value("count_fp"));
        const QString iso = payload.value("created_time").toString();
        t.ts_ms = qint64(payload.value("ts_ms").toDouble());
        if (t.ts_ms <= 0)
            t.ts_ms = iso.isEmpty() ? qint64(payload.value("ts").toVariant().toLongLong()) * 1000
                                    : QDateTime::fromString(iso, Qt::ISODate).toMSecsSinceEpoch();
        emit trade_received(t);
        openmarketterminal::datahub::DataHub::instance().publish(
            QStringLiteral("prediction:kalshi:trade:") + ticker,
            QVariant::fromValue(t));
        return;
    }

    if (type == QStringLiteral("market_lifecycle_v2") ||
        type == QStringLiteral("market_lifecycle")) {
        if (ticker.isEmpty()) return;
        QString status = payload.value("status").toString();
        if (status.isEmpty()) status = payload.value("market_status").toString();
        if (status.isEmpty()) status = payload.value("event_type").toString();
        if (status.isEmpty()) status = payload.value("type").toString();
        emit market_lifecycle_changed(ticker, status);
        emit market_lifecycle_event(ticker, status, payload);
        return;
    }

    if (type == QStringLiteral("fill") || type == QStringLiteral("user_orders") ||
        type == QStringLiteral("market_positions")) {
        emit account_event(type, payload);
        return;
    }

    if (type == QStringLiteral("cfbenchmarks_value")) {
        QJsonObject data = payload;
        const QString encoded = payload.value(QStringLiteral("data")).toString();
        if (!encoded.isEmpty()) {
            const QJsonDocument nested = QJsonDocument::fromJson(encoded.toUtf8());
            if (nested.isObject()) {
                const QJsonObject parsed = nested.object();
                for (auto it = parsed.constBegin(); it != parsed.constEnd(); ++it)
                    data.insert(it.key(), it.value());
            }
        }
        const QString index = data.value(QStringLiteral("id")).toString(
            payload.value(QStringLiteral("index_id")).toString()).toUpper();
        const double value = kalshi_fp_to_double(data.value(QStringLiteral("value")));
        qint64 ts_ms = data.value(QStringLiteral("time")).toVariant().toLongLong();
        if (ts_ms <= 0) ts_ms = data.value(QStringLiteral("ts_ms")).toVariant().toLongLong();
        if (ts_ms <= 0) ts_ms = QDateTime::currentMSecsSinceEpoch();
        if (!index.isEmpty() && value > 0.0)
            emit cf_benchmark_event(index, value, ts_ms, data);
        return;
    }

    if (type != QStringLiteral("orderbook_snapshot") &&
        type != QStringLiteral("orderbook_delta")) {
        return;
    }
    if (ticker.isEmpty()) return;

    const qint64 seq = qint64(obj.value("seq").toDouble());
    emit orderbook_event(type, ticker, seq, payload);
    qint64 ts_ms = qint64(payload.value("ts_ms").toDouble());
    if (ts_ms <= 0) ts_ms = qint64(payload.value("ts").toVariant().toLongLong()) * 1000;
    if (ts_ms <= 0) ts_ms = QDateTime::currentMSecsSinceEpoch();
    auto& state = books_[ticker];

    const auto parse_levels = [](const QJsonArray& levels, QMap<int, double>* out,
                                 bool yes_price_to_no_price = false) {
        out->clear();
        for (const auto& value : levels) {
            const auto level = value.toArray();
            if (level.size() < 2) continue;
            double parsed_price = kalshi_fp_to_double(level[0]);
            if (yes_price_to_no_price) parsed_price = 1.0 - parsed_price;
            const int price = qRound(parsed_price * 10000.0);
            const double size = kalshi_fp_to_double(level[1]);
            if (price > 0 && size > 0.0) out->insert(price, size);
        }
    };

    if (type == QStringLiteral("orderbook_snapshot")) {
        QJsonArray yes = payload.value("yes_dollars_fp").toArray();
        QJsonArray no = payload.value("no_dollars_fp").toArray();
        if (yes.isEmpty()) yes = payload.value("yes_dollars").toArray();
        if (no.isEmpty()) no = payload.value("no_dollars").toArray();
        parse_levels(yes, &state.yes_bids);
        parse_levels(no, &state.no_bids, true);
        if (seq > 0) orderbook_sequence_ = seq;
        state.has_snapshot = true;
        schedule_book_publish(ticker, ts_ms);
        return;
    }

    if (!state.has_snapshot || (seq > 0 && orderbook_sequence_ > 0 && seq != orderbook_sequence_ + 1)) {
        for (auto it = books_.begin(); it != books_.end(); ++it) {
            it->has_snapshot = false;
            it->yes_bids.clear();
            it->no_bids.clear();
        }
        orderbook_sequence_ = 0;
        for (const auto& subscribed : subscribed_tickers_) request_orderbook_snapshot(subscribed);
        return;
    }

    const QString side = payload.value("side").toString().toLower();
    double parsed_price = kalshi_fp_to_double(payload.value("price_dollars"));
    if (side == QStringLiteral("no")) parsed_price = 1.0 - parsed_price;
    const int price = qRound(parsed_price * 10000.0);
    const double delta = kalshi_fp_to_double(payload.value("delta_fp"));
    QMap<int, double>& levels = side == QStringLiteral("no") ? state.no_bids : state.yes_bids;
    const double next = levels.value(price) + delta;
    if (price > 0) {
        if (next <= 1e-9) levels.remove(price);
        else levels.insert(price, next);
    }
    orderbook_sequence_ = seq > 0 ? seq : orderbook_sequence_ + 1;
    schedule_book_publish(ticker, ts_ms);
}

// ── Hub publish helpers ─────────────────────────────────────────────────────

void KalshiWsClient::publish_price(const QString& asset_id, double price) {
    emit price_updated(asset_id, price);
    const QString topic = QStringLiteral("prediction:kalshi:price:") + asset_id;
    openmarketterminal::datahub::DataHub::instance().publish(topic, QVariant(price));
}

void KalshiWsClient::publish_orderbook(const QString& asset_id, const pr::PredictionOrderBook& book) {
    emit orderbook_updated(asset_id, book);
    const QString topic = QStringLiteral("prediction:kalshi:orderbook:") + asset_id;
    openmarketterminal::datahub::DataHub::instance().publish(topic, QVariant::fromValue(book));
}

// ── DataHub producer ────────────────────────────────────────────────────────

QStringList KalshiWsClient::topic_patterns() const {
    return {
        QStringLiteral("prediction:kalshi:price:*"),
        QStringLiteral("prediction:kalshi:orderbook:*"),
    };
}

void KalshiWsClient::refresh(const QStringList& /*topics*/) {
    // push_only: scheduler never calls this.
}

void KalshiWsClient::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = openmarketterminal::datahub::DataHub::instance();
    hub.register_producer(this);

    openmarketterminal::datahub::TopicPolicy push_only;
    push_only.push_only = true;
    push_only.ttl_ms = 0;
    push_only.min_interval_ms = 0;

    hub.set_policy_pattern(QStringLiteral("prediction:kalshi:price:*"), push_only);
    hub.set_policy_pattern(QStringLiteral("prediction:kalshi:orderbook:*"), push_only);

    hub_registered_ = true;
    LOG_INFO("KalshiWS",
             "Registered with DataHub (prediction:kalshi:price:*, prediction:kalshi:orderbook:*)");
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
