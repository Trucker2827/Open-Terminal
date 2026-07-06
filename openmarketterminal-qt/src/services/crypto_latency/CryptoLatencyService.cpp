#include "services/crypto_latency/CryptoLatencyService.h"

#include "datahub/DataHub.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>
#include <QVariant>
#include <QWebSocket>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::crypto_latency {

namespace {

qint64 now_ms() {
    return QDateTime::currentMSecsSinceEpoch();
}

double as_double(const QJsonValue& v) {
    if (v.isDouble())
        return v.toDouble();
    bool ok = false;
    const double d = v.toString().toDouble(&ok);
    return ok ? d : 0.0;
}

qint64 as_i64(const QJsonValue& v) {
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
    bool ok = false;
    const qint64 n = v.toString().toLongLong(&ok);
    return ok ? n : 0;
}

qint64 parse_time_ms(const QJsonValue& v) {
    if (v.isDouble())
        return as_i64(v);
    const QString s = v.toString().trimmed();
    if (s.isEmpty())
        return 0;
    bool ok = false;
    const qint64 raw = s.toLongLong(&ok);
    if (ok)
        return raw;
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (dt.isValid())
        return dt.toMSecsSinceEpoch();
    const QDateTime dt2 = QDateTime::fromString(s, Qt::ISODate);
    return dt2.isValid() ? dt2.toMSecsSinceEpoch() : 0;
}

QString binance_pair(const QString& symbol) {
    QString s = symbol.toUpper();
    s.remove('/');
    s.remove('-');
    if (s.endsWith(QStringLiteral("USD")) && !s.endsWith(QStringLiteral("USDT")))
        s += QStringLiteral("T");
    return s.toLower();
}

QString kraken_pair(const QString& symbol) {
    const QString s = CryptoLatencyService::normalize_symbol(symbol);
    if (s == QStringLiteral("BTC-USD"))
        return QStringLiteral("XBT/USD");
    if (s == QStringLiteral("ETH-USD"))
        return QStringLiteral("ETH/USD");
    if (s == QStringLiteral("SOL-USD"))
        return QStringLiteral("SOL/USD");
    return s;
}

QString topic_symbol(const QString& symbol) {
    return CryptoLatencyService::normalize_symbol(symbol).replace('-', '/');
}

} // namespace

CryptoLatencyService::CryptoLatencyService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<CryptoLatencyTick>();
    qRegisterMetaType<CryptoLatencySourceState>();
    qRegisterMetaType<CryptoLatencySnapshot>();
}

CryptoLatencyService::~CryptoLatencyService() {
    stop();
}

QStringList CryptoLatencyService::default_sources() {
    return {QStringLiteral("coinbase"), QStringLiteral("kraken"), QStringLiteral("binanceus"),
            QStringLiteral("bitcointicker")};
}

QStringList CryptoLatencyService::supported_sources() {
    return {QStringLiteral("coinbase"), QStringLiteral("kraken"), QStringLiteral("binanceus"),
            QStringLiteral("binance"), QStringLiteral("bitcointicker")};
}

QString CryptoLatencyService::normalize_symbol(QString symbol) {
    QString s = symbol.trimmed().toUpper();
    s.replace('/', '-');
    if (s == QStringLiteral("BTCUSDT") || s == QStringLiteral("BTCUSD"))
        return QStringLiteral("BTC-USD");
    if (s == QStringLiteral("ETHUSDT") || s == QStringLiteral("ETHUSD"))
        return QStringLiteral("ETH-USD");
    if (s == QStringLiteral("SOLUSDT") || s == QStringLiteral("SOLUSD"))
        return QStringLiteral("SOL-USD");
    if (!s.contains('-') && s.size() <= 6)
        s += QStringLiteral("-USD");
    return s;
}

QJsonObject CryptoLatencyService::tick_to_json(const CryptoLatencyTick& t) {
    return QJsonObject{{QStringLiteral("source"), t.source},
                       {QStringLiteral("symbol"), t.symbol},
                       {QStringLiteral("venue_symbol"), t.venue_symbol},
                       {QStringLiteral("price"), t.price},
                       {QStringLiteral("best_bid"), t.best_bid},
                       {QStringLiteral("best_ask"), t.best_ask},
                       {QStringLiteral("bid_size"), t.bid_size},
                       {QStringLiteral("ask_size"), t.ask_size},
                       {QStringLiteral("exchange_ts_ms"), QString::number(t.exchange_ts_ms)},
                       {QStringLiteral("received_ts_ms"), QString::number(t.received_ts_ms)},
                       {QStringLiteral("age_ms"), t.received_ts_ms > 0 ? now_ms() - t.received_ts_ms : -1},
                       {QStringLiteral("sequence"), QString::number(t.sequence)}};
}

QJsonObject CryptoLatencyService::source_to_json(const CryptoLatencySourceState& s) {
    return QJsonObject{{QStringLiteral("source"), s.source},
                       {QStringLiteral("status"), s.status},
                       {QStringLiteral("error"), s.error},
                       {QStringLiteral("last_message_type"), s.last_message_type},
                       {QStringLiteral("connected_at_ms"), QString::number(s.connected_at_ms)},
                       {QStringLiteral("last_message_ms"), QString::number(s.last_message_ms)},
                       {QStringLiteral("last_tick_ms"), QString::number(s.last_tick_ms)},
                       {QStringLiteral("age_ms"), s.last_tick_ms > 0 ? now_ms() - s.last_tick_ms : -1},
                       {QStringLiteral("raw_messages"), s.raw_messages},
                       {QStringLiteral("ticks"), s.ticks}};
}

QJsonObject CryptoLatencyService::snapshot_to_json(const CryptoLatencySnapshot& s) {
    QJsonArray ticks;
    for (const auto& t : s.latest_ticks)
        ticks.append(tick_to_json(t));
    QJsonArray sources;
    for (const auto& state : s.sources)
        sources.append(source_to_json(state));
    return QJsonObject{{QStringLiteral("symbol"), s.symbol},
                       {QStringLiteral("freshest_source"), s.freshest_source},
                       {QStringLiteral("freshest_age_ms"), s.freshest_age_ms},
                       {QStringLiteral("min_price"), s.min_price},
                       {QStringLiteral("max_price"), s.max_price},
                       {QStringLiteral("mid_price"), s.mid_price},
                       {QStringLiteral("cross_source_spread_bps"), s.cross_source_spread_bps},
                       {QStringLiteral("newest_tick_ms"), QString::number(s.newest_tick_ms)},
                       {QStringLiteral("oldest_tick_ms"), QString::number(s.oldest_tick_ms)},
                       {QStringLiteral("live_sources"), s.live_sources},
                       {QStringLiteral("total_ticks"), s.total_ticks},
                       {QStringLiteral("ticks"), ticks},
                       {QStringLiteral("sources"), sources}};
}

void CryptoLatencyService::start(const QString& symbol, const QStringList& sources) {
    stop();
    symbol_ = normalize_symbol(symbol);
    running_ = true;
    sequence_ = 0;

    QStringList wanted = sources;
    if (wanted.isEmpty())
        wanted = default_sources();
    for (QString source : wanted) {
        source = source.trimmed().toLower();
        if (!supported_sources().contains(source)) {
            set_state(source, QStringLiteral("unsupported"), QStringLiteral("unsupported source"));
            continue;
        }
        const Feed feed = make_feed(source, symbol_);
        feeds_.insert(source, feed);
        set_state(source, QStringLiteral("connecting"));
        open_feed(feed);
    }
}

void CryptoLatencyService::stop() {
    running_ = false;
    for (auto it = feeds_.begin(); it != feeds_.end(); ++it) {
        if (it->socket) {
            it->socket->close();
            it->socket->deleteLater();
            it->socket = nullptr;
        }
        if (it->tcp_socket) {
            it->tcp_socket->close();
            it->tcp_socket->deleteLater();
            it->tcp_socket = nullptr;
        }
    }
    feeds_.clear();
    terminal_states_.clear();
}

CryptoLatencyService::Feed CryptoLatencyService::make_feed(const QString& source, const QString& symbol) const {
    Feed f;
    f.source = source;
    if (source == QStringLiteral("coinbase")) {
        f.url = QStringLiteral("wss://ws-feed.exchange.coinbase.com");
        f.venue_symbol = normalize_symbol(symbol);
    } else if (source == QStringLiteral("kraken")) {
        f.url = QStringLiteral("wss://ws.kraken.com/v2");
        f.venue_symbol = kraken_pair(symbol);
    } else if (source == QStringLiteral("bitcointicker")) {
        f.url = QStringLiteral("ticker.bitcointicker.co:10080");
        f.venue_symbol = QStringLiteral("bitcointicker:coinbase-usd");
    } else if (source == QStringLiteral("binanceus")) {
        f.venue_symbol = binance_pair(symbol);
        f.url = QStringLiteral("wss://stream.binance.us:9443/stream?streams=%1@trade/%1@bookTicker")
                    .arg(f.venue_symbol);
    } else {
        f.venue_symbol = binance_pair(symbol);
        f.url = QStringLiteral("wss://stream.binance.com:9443/stream?streams=%1@trade/%1@bookTicker")
                    .arg(f.venue_symbol);
    }
    return f;
}

void CryptoLatencyService::open_feed(const Feed& feed) {
    if (feed.source == QStringLiteral("bitcointicker")) {
        open_tcp_feed(feed);
        return;
    }

    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    Feed stored = feed;
    stored.socket = socket;
    feeds_.insert(feed.source, stored);

    connect(socket, &QWebSocket::connected, this, [this, source = feed.source, venue = feed.venue_symbol, socket]() {
        set_state(source, QStringLiteral("connected"));
        if (source == QStringLiteral("coinbase")) {
            QJsonObject msg{{QStringLiteral("type"), QStringLiteral("subscribe")},
                            {QStringLiteral("product_ids"), QJsonArray{venue}},
                            {QStringLiteral("channels"), QJsonArray{QStringLiteral("matches"),
                                                                    QStringLiteral("ticker")}}};
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        } else if (source == QStringLiteral("kraken")) {
            for (const QString& channel : {QStringLiteral("trade"), QStringLiteral("ticker")}) {
                QJsonObject params{{QStringLiteral("channel"), channel},
                                   {QStringLiteral("symbol"), QJsonArray{venue}},
                                   {QStringLiteral("snapshot"), false}};
                QJsonObject msg{{QStringLiteral("method"), QStringLiteral("subscribe")},
                                {QStringLiteral("params"), params}};
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
            }
        }
    });
    connect(socket, &QWebSocket::disconnected, this, [this, source = feed.source]() {
        if (running_)
            set_state(source, QStringLiteral("disconnected"));
    });
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, source = feed.source, venue = feed.venue_symbol](const QString& text) {
                handle_text(source, venue, text);
            });
    connect(socket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::errorOccurred),
            this, [this, source = feed.source, socket](QAbstractSocket::SocketError) {
                set_state(source, QStringLiteral("error"), socket->errorString());
            });
    socket->open(QUrl(feed.url));
}

void CryptoLatencyService::open_tcp_feed(const Feed& feed) {
    auto* socket = new QTcpSocket(this);
    Feed stored = feed;
    stored.tcp_socket = socket;
    feeds_.insert(feed.source, stored);

    TerminalState term;
    term.rows = QStringList(12, QString(96, QLatin1Char(' ')));
    terminal_states_.insert(feed.source, term);

    connect(socket, &QTcpSocket::connected, this, [this, source = feed.source]() {
        set_state(source, QStringLiteral("connected"));
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, source = feed.source]() {
        if (running_)
            set_state(source, QStringLiteral("disconnected"));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, source = feed.source, venue = feed.venue_symbol, socket]() {
        handle_tcp_bytes(source, venue, socket->readAll());
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, source = feed.source, socket](QAbstractSocket::SocketError) {
        set_state(source, QStringLiteral("error"), socket->errorString());
    });
    socket->connectToHost(QStringLiteral("ticker.bitcointicker.co"), 10080);
}

void CryptoLatencyService::handle_text(const QString& source, const QString& venue_symbol, const QString& text) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("data")) && obj.value(QStringLiteral("data")).isObject())
        obj = obj.value(QStringLiteral("data")).toObject();
    QString message_type;
    if (source == QStringLiteral("binance") || source == QStringLiteral("binanceus")) {
        message_type = obj.value(QStringLiteral("e")).toString();
    } else if (source == QStringLiteral("coinbase")) {
        message_type = obj.value(QStringLiteral("channel")).toString();
        if (message_type.isEmpty())
            message_type = obj.value(QStringLiteral("type")).toString();
    } else if (source == QStringLiteral("kraken")) {
        message_type = obj.value(QStringLiteral("channel")).toString();
        if (message_type.isEmpty())
            message_type = obj.value(QStringLiteral("method")).toString();
        if (message_type.isEmpty())
            message_type = obj.value(QStringLiteral("event")).toString();
    }
    note_message(source, message_type.isEmpty() ? QStringLiteral("json") : message_type);

    CryptoLatencyTick tick;
    tick.source = source;
    tick.symbol = symbol_;
    tick.venue_symbol = venue_symbol;
    tick.received_ts_ms = now_ms();

    if (source == QStringLiteral("binance") || source == QStringLiteral("binanceus")) {
        const QString event = obj.value(QStringLiteral("e")).toString();
        if (event == QStringLiteral("bookTicker")) {
            tick.best_bid = as_double(obj.value(QStringLiteral("b")));
            tick.best_ask = as_double(obj.value(QStringLiteral("a")));
            tick.bid_size = as_double(obj.value(QStringLiteral("B")));
            tick.ask_size = as_double(obj.value(QStringLiteral("A")));
            tick.price = (tick.best_bid > 0.0 && tick.best_ask > 0.0)
                             ? (tick.best_bid + tick.best_ask) / 2.0
                             : 0.0;
            tick.exchange_ts_ms = now_ms();
        } else if (event == QStringLiteral("trade")) {
            tick.price = as_double(obj.value(QStringLiteral("p")));
            tick.exchange_ts_ms = as_i64(obj.value(QStringLiteral("T")));
        } else {
            return;
        }
    } else if (source == QStringLiteral("coinbase")) {
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("ticker")) {
            tick.price = as_double(obj.value(QStringLiteral("price")));
            tick.best_bid = as_double(obj.value(QStringLiteral("best_bid")));
            tick.best_ask = as_double(obj.value(QStringLiteral("best_ask")));
            tick.bid_size = as_double(obj.value(QStringLiteral("best_bid_size")));
            tick.ask_size = as_double(obj.value(QStringLiteral("best_ask_size")));
            tick.exchange_ts_ms = parse_time_ms(obj.value(QStringLiteral("time")));
            if (tick.price > 0.0)
                emit_tick(tick);
            return;
        }
        if (type == QStringLiteral("match") || type == QStringLiteral("last_match")) {
            tick.price = as_double(obj.value(QStringLiteral("price")));
            tick.exchange_ts_ms = parse_time_ms(obj.value(QStringLiteral("time")));
            if (tick.price > 0.0)
                emit_tick(tick);
            return;
        }
        const QJsonArray events = obj.value(QStringLiteral("events")).toArray();
        for (const auto& ev : events) {
            const QJsonObject event = ev.toObject();
            const QJsonArray trades = event.value(QStringLiteral("trades")).toArray();
            for (const auto& tv : trades) {
                const QJsonObject trade = tv.toObject();
                tick.price = as_double(trade.value(QStringLiteral("price")));
                tick.exchange_ts_ms = parse_time_ms(trade.value(QStringLiteral("time")));
                tick.best_bid = as_double(trade.value(QStringLiteral("best_bid")));
                tick.best_ask = as_double(trade.value(QStringLiteral("best_ask")));
                if (tick.price > 0.0)
                    emit_tick(tick);
            }
            const QJsonArray tickers = event.value(QStringLiteral("tickers")).toArray();
            for (const auto& tv : tickers) {
                const QJsonObject ticker = tv.toObject();
                tick.price = as_double(ticker.value(QStringLiteral("price")));
                tick.best_bid = as_double(ticker.value(QStringLiteral("best_bid")));
                tick.best_ask = as_double(ticker.value(QStringLiteral("best_ask")));
                tick.bid_size = as_double(ticker.value(QStringLiteral("best_bid_quantity")));
                tick.ask_size = as_double(ticker.value(QStringLiteral("best_ask_quantity")));
                tick.exchange_ts_ms = parse_time_ms(ticker.value(QStringLiteral("time")));
                if (tick.price > 0.0)
                    emit_tick(tick);
            }
        }
        return;
    } else if (source == QStringLiteral("kraken")) {
        const QString channel = obj.value(QStringLiteral("channel")).toString();
        const QJsonArray data = obj.value(QStringLiteral("data")).toArray();
        if (channel == QStringLiteral("ticker")) {
            for (const auto& tv : data) {
                const QJsonObject ticker = tv.toObject();
                tick.price = as_double(ticker.value(QStringLiteral("last")));
                if (tick.price <= 0.0)
                    tick.price = as_double(ticker.value(QStringLiteral("price")));
                tick.best_bid = as_double(ticker.value(QStringLiteral("bid")));
                tick.best_ask = as_double(ticker.value(QStringLiteral("ask")));
                tick.bid_size = as_double(ticker.value(QStringLiteral("bid_qty")));
                tick.ask_size = as_double(ticker.value(QStringLiteral("ask_qty")));
                tick.exchange_ts_ms = parse_time_ms(ticker.value(QStringLiteral("timestamp")));
                if (tick.price > 0.0)
                    emit_tick(tick);
            }
            return;
        }
        if (channel != QStringLiteral("trade"))
            return;
        for (const auto& tv : data) {
            const QJsonObject trade = tv.toObject();
            tick.price = as_double(trade.value(QStringLiteral("price")));
            tick.exchange_ts_ms = parse_time_ms(trade.value(QStringLiteral("timestamp")));
            if (tick.price > 0.0)
                emit_tick(tick);
        }
        return;
    }

    if (tick.price > 0.0)
        emit_tick(tick);
}

void CryptoLatencyService::handle_tcp_bytes(const QString& source,
                                            const QString& venue_symbol,
                                            const QByteArray& bytes) {
    note_message(source, QStringLiteral("tcp"));
    auto term = terminal_states_.value(source);
    if (term.rows.isEmpty())
        term.rows = QStringList(12, QString(96, QLatin1Char(' ')));

    const QString text = QString::fromLatin1(bytes);
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (term.in_escape) {
            term.escape.append(ch);
            if ((term.escape.size() > 1 && u >= 0x40 && u <= 0x7e) || term.escape.size() > 32) {
                apply_terminal_escape(term, term.escape);
                term.escape.clear();
                term.in_escape = false;
            }
            continue;
        }
        if (u == 0x1b) {
            term.in_escape = true;
            term.escape.clear();
            continue;
        }
        if (ch == QLatin1Char('\r')) {
            term.col = 0;
            continue;
        }
        if (ch == QLatin1Char('\n')) {
            term.row = qBound(0, term.row + 1, term.rows.size() - 1);
            term.col = 0;
            continue;
        }
        if (u < 32 || u == 127)
            continue;
        if (term.row < 0 || term.row >= term.rows.size())
            continue;
        if (term.col >= term.rows[term.row].size())
            term.rows[term.row].resize(term.col + 16, QLatin1Char(' '));
        term.rows[term.row][term.col] = ch;
        term.col += 1;
    }

    terminal_states_.insert(source, term);
    const double price = parse_bitcointicker_price(term);
    if (price <= 0.0)
        return;
    double bid = 0.0;
    double ask = 0.0;
    parse_bitcointicker_book(term, &bid, &ask);
    const auto previous = latest_.value(source);
    if (previous.price > 0.0 && std::abs(previous.price - price) < 0.000001 &&
        std::abs(previous.best_bid - bid) < 0.000001 &&
        std::abs(previous.best_ask - ask) < 0.000001)
        return;

    CryptoLatencyTick tick;
    tick.source = source;
    tick.symbol = symbol_;
    tick.venue_symbol = venue_symbol;
    tick.price = price;
    tick.best_bid = bid;
    tick.best_ask = ask;
    tick.exchange_ts_ms = 0;
    tick.received_ts_ms = now_ms();
    emit_tick(tick);
}

void CryptoLatencyService::apply_terminal_escape(TerminalState& term, const QString& seq) {
    if (!seq.startsWith(QLatin1Char('[')))
        return;
    const QChar cmd = seq.back();
    QString body = seq.mid(1, seq.size() - 2);
    body.remove(QLatin1Char('?'));
    const QStringList parts = body.split(';', Qt::SkipEmptyParts);

    if (cmd == QLatin1Char('J') && body == QLatin1String("2")) {
        term.rows = QStringList(12, QString(96, QLatin1Char(' ')));
        term.row = 0;
        term.col = 0;
        return;
    }
    if (cmd == QLatin1Char('H') || cmd == QLatin1Char('f')) {
        int row = 1;
        int col = 1;
        if (!parts.isEmpty())
            row = parts.value(0).toInt();
        if (parts.size() > 1)
            col = parts.value(1).toInt();
        term.row = qBound(0, row - 1, qMax(0, term.rows.size() - 1));
        term.col = qMax(0, col - 1);
        return;
    }
    if (cmd == QLatin1Char('C')) {
        const int amount = parts.isEmpty() ? body.toInt() : parts.first().toInt();
        term.col += qMax(1, amount);
    }
}

double CryptoLatencyService::parse_bitcointicker_price(const TerminalState& term) const {
    static const QRegularExpression coinbase_re(
        QStringLiteral("Coinbase:\\$\\s*([0-9]+(?:\\.[0-9]+)?)"));
    for (const QString& row : term.rows) {
        const auto match = coinbase_re.match(row);
        if (!match.hasMatch())
            continue;
        bool ok = false;
        const double price = match.captured(1).toDouble(&ok);
        if (ok && price > 0.0)
            return price;
    }
    return 0.0;
}

bool CryptoLatencyService::parse_bitcointicker_book(const TerminalState& term, double* bid, double* ask) const {
    if (bid)
        *bid = 0.0;
    if (ask)
        *ask = 0.0;
    static const QRegularExpression bid_re(QStringLiteral("\\bBid\\s*:?\\s*([0-9]+(?:\\.[0-9]+)?)"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ask_re(QStringLiteral("\\bAsk\\s*:?\\s*([0-9]+(?:\\.[0-9]+)?)"),
                                           QRegularExpression::CaseInsensitiveOption);
    bool found = false;
    for (const QString& row : term.rows) {
        const auto bm = bid_re.match(row);
        if (bm.hasMatch() && bid) {
            bool ok = false;
            const double v = bm.captured(1).toDouble(&ok);
            if (ok && v > 0.0) {
                *bid = v;
                found = true;
            }
        }
        const auto am = ask_re.match(row);
        if (am.hasMatch() && ask) {
            bool ok = false;
            const double v = am.captured(1).toDouble(&ok);
            if (ok && v > 0.0) {
                *ask = v;
                found = true;
            }
        }
    }
    return found;
}

void CryptoLatencyService::note_message(const QString& source, const QString& message_type) {
    auto state = states_.value(source);
    state.source = source;
    state.last_message_type = message_type;
    state.last_message_ms = now_ms();
    state.raw_messages += 1;
    states_.insert(source, state);
}

void CryptoLatencyService::emit_tick(CryptoLatencyTick tick) {
    tick.sequence = ++sequence_;
    latest_.insert(tick.source, tick);
    auto state = states_.value(tick.source);
    state.source = tick.source;
    state.status = QStringLiteral("live");
    state.error.clear();
    state.last_tick_ms = tick.received_ts_ms;
    state.ticks += 1;
    states_.insert(tick.source, state);

    const QString base = QStringLiteral("crypto:latency:") + topic_symbol(tick.symbol);
    datahub::DataHub::instance().publish(base + QLatin1Char(':') + tick.source,
                                         QVariant::fromValue(tick_to_json(tick)));
    datahub::DataHub::instance().publish(base + QStringLiteral(":snapshot"),
                                         QVariant::fromValue(snapshot_to_json(snapshot())));

    emit tick_received(tick);
    emit source_state_changed(state);
    emit snapshot_changed(snapshot());
}

void CryptoLatencyService::set_state(const QString& source, const QString& status, const QString& error) {
    CryptoLatencySourceState state = states_.value(source);
    state.source = source;
    state.status = status;
    state.error = error;
    if (status == QStringLiteral("connected") && state.connected_at_ms <= 0)
        state.connected_at_ms = now_ms();
    states_.insert(source, state);
    emit source_state_changed(state);
}

CryptoLatencySnapshot CryptoLatencyService::snapshot() const {
    CryptoLatencySnapshot s;
    s.symbol = symbol_;
    s.sources = states_.values().toVector();
    std::sort(s.sources.begin(), s.sources.end(), [](const auto& a, const auto& b) {
        return a.source < b.source;
    });
    s.latest_ticks = latest_.values().toVector();
    std::sort(s.latest_ticks.begin(), s.latest_ticks.end(), [](const auto& a, const auto& b) {
        return a.received_ts_ms > b.received_ts_ms;
    });
    const qint64 now = now_ms();
    for (const auto& t : s.latest_ticks) {
        if (t.price <= 0.0)
            continue;
        s.total_ticks += 1;
        if (s.min_price <= 0.0 || t.price < s.min_price)
            s.min_price = t.price;
        if (t.price > s.max_price)
            s.max_price = t.price;
        if (t.received_ts_ms > s.newest_tick_ms) {
            s.newest_tick_ms = t.received_ts_ms;
            s.freshest_source = t.source;
        }
        if (s.oldest_tick_ms <= 0 || t.received_ts_ms < s.oldest_tick_ms)
            s.oldest_tick_ms = t.received_ts_ms;
    }
    for (const auto& state : s.sources) {
        if (state.last_tick_ms > 0)
            s.live_sources += 1;
    }
    if (s.newest_tick_ms > 0)
        s.freshest_age_ms = now - s.newest_tick_ms;
    if (s.min_price > 0.0 && s.max_price > 0.0) {
        s.mid_price = (s.min_price + s.max_price) / 2.0;
        if (s.mid_price > 0.0)
            s.cross_source_spread_bps = ((s.max_price - s.min_price) / s.mid_price) * 10000.0;
    }
    return s;
}

} // namespace openmarketterminal::services::crypto_latency
