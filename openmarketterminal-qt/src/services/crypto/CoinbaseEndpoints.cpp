// Definitions for CoinbaseEndpoints — kept OUT of the header on purpose.
// The all-inline first version of this header front-end-ICE'd MSVC
// (C1001 msc1.cpp:1589) when parsed into the ~27k-line CommandDispatch.cpp
// TU (v0.3.29 Windows installer, 4x deterministic; same compiler had built
// v0.3.28 green). Keeping only declarations in the header sidesteps it.

#include "services/crypto/CoinbaseEndpoints.h"

namespace openmarketterminal::services::crypto {

double adv_number(const QJsonValue& v) {
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        return ok ? d : 0.0;
    }
    return 0.0;
}

QUrl advanced_ticker_url(const QString& product) {
    QUrl url(QStringLiteral("https://api.coinbase.com/api/v3/brokerage/market/products/%1/ticker").arg(product));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    url.setQuery(q);
    return url;
}

QUrl advanced_candles_url(const QString& product, qint64 start_s, qint64 end_s) {
    QUrl url(QStringLiteral("https://api.coinbase.com/api/v3/brokerage/market/products/%1/candles").arg(product));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("start"), QString::number(start_s));
    q.addQueryItem(QStringLiteral("end"), QString::number(end_s));
    q.addQueryItem(QStringLiteral("granularity"), QStringLiteral("ONE_MINUTE"));
    url.setQuery(q);
    return url;
}

bool parse_advanced_ticker(const QJsonDocument& doc, double* price, double* bid, double* ask) {
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

QVector<AdvCandle> parse_advanced_candles(const QJsonDocument& doc) {
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

QJsonObject advanced_ws_subscribe(const QStringList& product_ids, const QString& channel) {
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
