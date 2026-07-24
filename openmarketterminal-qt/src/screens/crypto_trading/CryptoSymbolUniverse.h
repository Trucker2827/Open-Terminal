#pragma once
// Per-exchange symbol universe — the display pair IS the wire pair.
// Kills the Coinbase "/USDT displayed, /USD on the wire" remap fiction.
// Pure: no Qt widgets, no I/O.

#include <QString>
#include <QStringList>

namespace openmarketterminal::crypto {

/// Native quote currency for an exchange's default spot universe.
/// USD-quoted venues list explicitly; everything else (binance, okx, bybit,
/// kucoin, bitget, gate, mexc, unknown) conservatively keeps USDT.
inline QString quote_currency_for(const QString& exchange_id) {
    if (exchange_id == QLatin1String("coinbase") || exchange_id == QLatin1String("kraken"))
        return QStringLiteral("USD");
    return QStringLiteral("USDT");
}

/// Default watchlist bases, quoted in the exchange's native currency.
inline QStringList default_watchlist_for(const QString& exchange_id, bool bitcoin_focus) {
    const QString q = quote_currency_for(exchange_id);
    if (bitcoin_focus)
        return {QStringLiteral("BTC/") + q};
    static const QStringList kBases = {
        QStringLiteral("BTC"),  QStringLiteral("ETH"),  QStringLiteral("SOL"),  QStringLiteral("BNB"),
        QStringLiteral("XRP"),  QStringLiteral("DOGE"), QStringLiteral("ADA"),  QStringLiteral("AVAX"),
        QStringLiteral("TON"),  QStringLiteral("LINK"), QStringLiteral("DOT"),  QStringLiteral("MATIC"),
        QStringLiteral("UNI"),  QStringLiteral("ATOM"), QStringLiteral("LTC"),  QStringLiteral("BCH"),
        QStringLiteral("APT"),  QStringLiteral("ARB"),  QStringLiteral("OP"),   QStringLiteral("SUI"),
        QStringLiteral("TRX"),  QStringLiteral("INJ"),  QStringLiteral("NEAR"), QStringLiteral("WIF"),
        QStringLiteral("PEPE"),
    };
    QStringList out;
    out.reserve(kBases.size());
    for (const QString& b : kBases)
        out << b + QLatin1Char('/') + q;
    return out;
}

/// Default primary symbol — BTC quoted in the venue's native currency for
/// both focus modes (bitcoin-focus differs only in the watchlist).
inline QString default_symbol_for(const QString& exchange_id, bool /*bitcoin_focus*/) {
    return QStringLiteral("BTC/") + quote_currency_for(exchange_id);
}

/// Persisted-state migration: remap ONLY a stale default-quote suffix to the
/// exchange's native quote. A user's explicit non-default pair (e.g.
/// BTC/USDC) and unknown shapes pass through untouched.
inline QString migrate_symbol(const QString& exchange_id, const QString& symbol) {
    const QString native = quote_currency_for(exchange_id);
    if (native != QLatin1String("USD"))
        return symbol; // only USD-native venues have a stale-USDT legacy to migrate
    const QString suffix = QStringLiteral("/USDT");
    if (!symbol.endsWith(suffix))
        return symbol;
    return symbol.left(symbol.size() - suffix.size()) + QStringLiteral("/") + native;
}

} // namespace openmarketterminal::crypto
