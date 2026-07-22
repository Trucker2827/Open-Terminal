#pragma once
// Coinbase Advanced Trade PUBLIC endpoints + parsers — DECLARATIONS ONLY.
//
// The legacy Coinbase Pro / Exchange API (api.exchange.coinbase.com,
// ws-feed.exchange.coinbase.com) is sunset; every Coinbase URL outside the
// ccxt daemon must come from here so a future endpoint change is one edit.
// All numeric fields in Advanced Trade responses arrive as JSON STRINGS;
// parsers accept a plain number too for forward compatibility.
//
// Definitions live in CoinbaseEndpoints.cpp — see the note there for why
// this header must stay body-free (MSVC front-end ICE in huge TUs).

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
double adv_number(const QJsonValue& v);

/// GET — latest trade + best bid/ask for a product (e.g. "BTC-USD").
QUrl advanced_ticker_url(const QString& product);

/// GET — 1m candles for [start_s, end_s] (unix SECONDS; max 300 candles/call).
QUrl advanced_candles_url(const QString& product, qint64 start_s, qint64 end_s);

/// Response shape: {"trades":[{"price":"...",...}], "best_bid":"...", "best_ask":"..."}
/// Returns false when no usable trade price is present.
bool parse_advanced_ticker(const QJsonDocument& doc, double* price, double* bid, double* ask);

struct AdvCandle {
    qint64 open_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

/// Response shape: {"candles":[{"start":"<unix_s>","low":...,...}]} — newest-first.
QVector<AdvCandle> parse_advanced_candles(const QJsonDocument& doc);

/// Advanced Trade WS subscribe frame — note the SINGLE "channel" string
/// (the legacy Exchange feed took a "channels" array).
QJsonObject advanced_ws_subscribe(const QStringList& product_ids, const QString& channel);

} // namespace openmarketterminal::services::crypto
