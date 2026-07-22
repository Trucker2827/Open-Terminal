#include "screens/crypto_trading/CryptoSymbolUniverse.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

class TstCryptoSymbolUniverse : public QObject {
    Q_OBJECT
  private slots:
    void quote_currency() {
        QCOMPARE(quote_currency_for("coinbase"), QString("USD"));
        QCOMPARE(quote_currency_for("kraken"), QString("USD"));
        QCOMPARE(quote_currency_for("binance"), QString("USDT"));
        QCOMPARE(quote_currency_for("okx"), QString("USDT"));
        QCOMPARE(quote_currency_for("unknown_exchange"), QString("USDT")); // conservative default
    }
    void default_symbol() {
        // Bitcoin-focus is venue-consistent: BTC quoted in the venue's native
        // currency (the old hardcoded BTC/USD-under-focus was Coinbase-implicit).
        QCOMPARE(default_symbol_for("coinbase", false), QString("BTC/USD"));
        QCOMPARE(default_symbol_for("binance", false), QString("BTC/USDT"));
        QCOMPARE(default_symbol_for("coinbase", true), QString("BTC/USD"));
        QCOMPARE(default_symbol_for("binance", true), QString("BTC/USDT"));
    }
    void watchlist_no_usdt_on_coinbase() {
        const QStringList wl = default_watchlist_for("coinbase", false);
        QVERIFY(wl.size() >= 20);
        for (const QString& s : wl)
            QVERIFY2(!s.endsWith("/USDT"), qPrintable(s));
        QVERIFY(wl.contains("BTC/USD"));
    }
    void watchlist_binance_keeps_usdt() {
        const QStringList wl = default_watchlist_for("binance", false);
        QVERIFY(wl.contains("BTC/USDT"));
    }
    void watchlist_bitcoin_focus() {
        QCOMPARE(default_watchlist_for("coinbase", true), QStringList{QStringLiteral("BTC/USD")});
        QCOMPARE(default_watchlist_for("binance", true), QStringList{QStringLiteral("BTC/USDT")});
    }
    void migrate_quote_suffix_only() {
        QCOMPARE(migrate_symbol("coinbase", "ETH/USDT"), QString("ETH/USD"));  // stale quote remapped
        QCOMPARE(migrate_symbol("coinbase", "ETH/USD"), QString("ETH/USD"));   // already native
        QCOMPARE(migrate_symbol("binance", "ETH/USDT"), QString("ETH/USDT"));  // no-op on USDT venues
        QCOMPARE(migrate_symbol("coinbase", "WEIRD"), QString("WEIRD"));       // unknown shape untouched
        QCOMPARE(migrate_symbol("coinbase", "BTC/USDC"), QString("BTC/USDC")); // explicit USDC pair untouched
    }
};

QTEST_MAIN(TstCryptoSymbolUniverse)
#include "tst_crypto_symbol_universe.moc"
