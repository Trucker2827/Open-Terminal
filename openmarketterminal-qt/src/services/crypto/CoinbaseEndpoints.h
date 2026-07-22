#pragma once
// Coinbase Advanced Trade PUBLIC endpoints + parsers.
//
// The legacy Coinbase Pro / Exchange API (api.exchange.coinbase.com,
// ws-feed.exchange.coinbase.com) is sunset; every Coinbase URL outside the
// ccxt daemon must come from here so a future endpoint change is one edit.
// All numeric fields in Advanced Trade responses arrive as JSON STRINGS;
// parsers accept a plain number too for forward compatibility.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

namespace openmarketterminal::services::crypto {

inline constexpr const char* kAdvancedTradeWsUrl = "wss://advanced-trade-ws.coinbase.com";
// Kept ONLY as a reconnect fallback while the legacy feed still answers.
inline constexpr const char* kLegacyExchangeWsUrl = "wss://ws-feed.exchange.coinbase.com";

/// Best-effort numeric read: Advanced Trade sends numbers as strings.
inline double adv_number(const QJsonValue& v) {
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        return ok ? d : 0.0;
    }
    return 0.0;
}

/// GET — latest trade + best bid/ask for a product (e.g. "BTC-USD").
inline QUrl advanced_ticker_url(const QString& product) {
    QUrl url(QStringLiteral("https://api.coinbase.com/api/v3/brokerage/market/products/%1/ticker").arg(product));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    url.setQuery(q);
    return url;
}

/// GET — 1m candles for [start_s, end_s] (unix SECONDS; max 300 candles/call).
inline QUrl advanced_candles_url(const QString& product, qint64 start_s, qint64 end_s) {
    QUrl url(QStringLiteral("https://api.coinbase.com/api/v3/brokerage/market/products/%1/candles").arg(product));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("start"), QString::number(start_s));
    q.addQueryItem(QStringLiteral("end"), QString::number(end_s));
    q.addQueryItem(QStringLiteral("granularity"), QStringLiteral("ONE_MINUTE"));
    url.setQuery(q);
    return url;
}

/// Response shape: {"trades":[{"price":"...",...}], "best_bid":"...", "best_ask":"..."}
/// Returns false when no usable trade price is present.
inline bool parse_advanced_ticker(const QJsonDocument& doc, double* price, double* bid, double* ask) {
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    const QJsonArray trades = o.value(QStringLiteral("trades")).toArray();
    if (trades.isEmpty())
        return false;
    const double p = adv_number(trades.at(0).toObject().value(QStringLiteral("price")));
    if (p <= 0)
        return false;
    if (price)
        *price = p;
    if (bid)
        *bid = adv_number(o.value(QStringLiteral("best_bid")));
    if (ask)
        *ask = adv_number(o.value(QStringLiteral("best_ask")));
    return true;
}

struct AdvCandle {
    qint64 open_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

/// Response shape: {"candles":[{"start":"<unix_s>","low":...,"high":...,
/// "open":...,"close":...,"volume":...}]} — newest-first, same as legacy.
inline QVector<AdvCandle> parse_advanced_candles(const QJsonDocument& doc) {
    QVector<AdvCandle> out;
    if (!doc.isObject())
        return out;
    for (const QJsonValue& v : doc.object().value(QStringLiteral("candles")).toArray()) {
        const QJsonObject c = v.toObject();
        AdvCandle a;
        a.open_ms = static_cast<qint64>(adv_number(c.value(QStringLiteral("start")))) * 1000;
        a.low = adv_number(c.value(QStringLiteral("low")));
        a.high = adv_number(c.value(QStringLiteral("high")));
        a.open = adv_number(c.value(QStringLiteral("open")));
        a.close = adv_number(c.value(QStringLiteral("close")));
        a.volume = adv_number(c.value(QStringLiteral("volume")));
        if (a.open_ms > 0 && a.close > 0)
            out.append(a);
    }
    return out;
}

/// Advanced Trade WS subscribe frame — note the SINGLE "channel" string
/// (the legacy Exchange feed took a "channels" array; sending that form to
/// the Advanced endpoint is silently ignored).
inline QJsonObject advanced_ws_subscribe(const QStringList& product_ids, const QString& channel) {
    QJsonArray ids;
    for (const QString& p : product_ids)
        ids.append(p);
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("subscribe")},
        {QStringLiteral("product_ids"), ids},
        {QStringLiteral("channel"), channel},
    };
}

} // namespace openmarketterminal::services::crypto
