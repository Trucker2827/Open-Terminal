// tst_crypto_account_source.cpp — Portfolio Account Sync, Task 9:
// CryptoAccountSource::fetch() over an injected balance-fetcher callback (no
// live exchange daemon needed). Covers:
//   1. Success — {"balances":{"BTC":{"total":0.3},"ETH":{"total":2},
//      "USD":{"total":1000}}} maps to 3 SyncedHoldings: BTC-USD / ETH-USD
//      coin holdings (has_cost_basis=false, broker_symbol="<CCY>/USD") + one
//      "$CASH:USD" cash line (has_cost_basis=false, avg_cost=1).
//   2. Failure — an {"error": ...} response (or a response missing
//      "balances") -> FetchResult{ok=false, holdings empty}.

#include <QtTest>

#include "services/portfolio/CryptoAccountSource.h"

#include <QJsonArray>
#include <QJsonObject>

using namespace openmarketterminal;
using namespace openmarketterminal::services;

namespace {

const portfolio::SyncedHolding* find_hold(const QVector<portfolio::SyncedHolding>& holdings, const QString& symbol) {
    for (const auto& h : holdings)
        if (h.canonical_symbol == symbol)
            return &h;
    return nullptr;
}

} // namespace

class TstCryptoAccountSource : public QObject {
    Q_OBJECT

  private slots:
    void fetch_maps_balances_to_holdings_and_cash() {
        CryptoAccountSource src([](const QString& exchange_id) -> QJsonObject {
            Q_UNUSED(exchange_id);
            QJsonObject balances;
            balances["BTC"] = QJsonObject{{"total", 0.3}};
            balances["ETH"] = QJsonObject{{"total", 2}};
            balances["USD"] = QJsonObject{{"total", 1000}};
            return QJsonObject{{"balances", balances}};
        });

        const auto r = src.fetch(AccountRef{"crypto:coinbase", "Coinbase", "USD"});
        QVERIFY(r.ok);
        QCOMPARE(r.holdings.size(), qsizetype{3});

        const auto* btc = find_hold(r.holdings, "BTC-USD");
        QVERIFY(btc);
        QCOMPARE(btc->quantity, 0.3);
        QVERIFY(!btc->has_cost_basis);
        QCOMPARE(btc->avg_cost, 0.0);
        QCOMPARE(btc->broker_symbol, QStringLiteral("BTC/USD"));
        QCOMPARE(btc->exchange, QStringLiteral("coinbase"));
        QCOMPARE(btc->native_currency, QStringLiteral("USD"));

        const auto* eth = find_hold(r.holdings, "ETH-USD");
        QVERIFY(eth);
        QCOMPARE(eth->quantity, 2.0);
        QCOMPARE(eth->broker_symbol, QStringLiteral("ETH/USD"));

        const auto* cash = find_hold(r.holdings, "$CASH:USD");
        QVERIFY(cash);
        QCOMPARE(cash->quantity, 1000.0);
        QVERIFY(!cash->has_cost_basis);
        QCOMPARE(cash->avg_cost, 1.0);
        QCOMPARE(cash->native_currency, QStringLiteral("USD"));
    }

    void fetch_uppercases_lowercase_ccy_keys_to_canonical_form() {
        // Guards against reconcile_mirror thrash: PortfolioRepository stores
        // symbols via .toUpper(), so a lowercase canonical_symbol from the
        // source would look "new" against the stored uppercase row forever.
        CryptoAccountSource src([](const QString& exchange_id) -> QJsonObject {
            Q_UNUSED(exchange_id);
            QJsonObject balances;
            balances["btc"] = QJsonObject{{"total", 0.3}};
            balances["usd"] = QJsonObject{{"total", 100}};
            return QJsonObject{{"balances", balances}};
        });

        const auto r = src.fetch(AccountRef{"crypto:coinbase", "Coinbase", "USD"});
        QVERIFY(r.ok);
        QCOMPARE(r.holdings.size(), qsizetype{2});

        const auto* btc = find_hold(r.holdings, "BTC-USD");
        QVERIFY(btc);
        QCOMPARE(btc->quantity, 0.3);

        const auto* cash = find_hold(r.holdings, "$CASH:USD");
        QVERIFY(cash);
        QCOMPARE(cash->quantity, 100.0);
    }

    void fetch_fails_without_partial_mirror_on_error_response() {
        CryptoAccountSource src([](const QString&) -> QJsonObject { return QJsonObject{{"error", "auth failed"}}; });

        const auto r = src.fetch(AccountRef{"crypto:coinbase", "Coinbase", "USD"});
        QVERIFY(!r.ok);
        QVERIFY(r.holdings.isEmpty());
        QVERIFY(!r.error.isEmpty());
    }

    void fetch_transactions_maps_trades_for_each_held_coin() {
        // Two coin holdings (BTC, ETH) + one $CASH holding — fetch_transactions
        // must query trades for the two coins and skip cash.
        QVector<portfolio::SyncedHolding> holdings;
        portfolio::SyncedHolding btc;
        btc.canonical_symbol = "BTC-USD";
        btc.broker_symbol = "BTC/USD";
        btc.exchange = "coinbase";
        holdings.append(btc);
        portfolio::SyncedHolding eth;
        eth.canonical_symbol = "ETH-USD";
        eth.broker_symbol = "ETH/USD";
        eth.exchange = "coinbase";
        holdings.append(eth);
        portfolio::SyncedHolding cash;
        cash.canonical_symbol = "$CASH:USD";
        holdings.append(cash);

        QStringList queried_symbols;
        CryptoAccountSource src(
            [](const QString&) -> QJsonObject { return {}; },
            [&](const QString&, const QString& symbol) -> QJsonObject {
                queried_symbols.append(symbol);
                QJsonArray trades;
                if (symbol == "BTC/USD") {
                    trades.append(QJsonObject{{"id", "t1"},
                                              {"side", "buy"},
                                              {"amount", 0.1},
                                              {"price", 50000.0},
                                              {"datetime", "2026-01-01T00:00:00Z"}});
                }
                return QJsonObject{{"trades", trades}};
            });

        const auto txs = src.fetch_transactions(AccountRef{"crypto:coinbase", "Coinbase", "USD"}, holdings);

        QCOMPARE(queried_symbols.size(), qsizetype{2});
        QVERIFY(queried_symbols.contains("BTC/USD"));
        QVERIFY(queried_symbols.contains("ETH/USD"));

        QCOMPARE(txs.size(), qsizetype{1});
        QCOMPARE(txs.first().external_id, QStringLiteral("coinbase:t1"));
        QCOMPARE(txs.first().symbol, QStringLiteral("BTC-USD"));
        QCOMPARE(txs.first().type, QStringLiteral("BUY"));
        QCOMPARE(txs.first().quantity, 0.1);
        QCOMPARE(txs.first().price, 50000.0);
        QCOMPARE(txs.first().date, QStringLiteral("2026-01-01T00:00:00Z"));
    }

    void fetch_fails_when_balances_key_missing() {
        CryptoAccountSource src([](const QString&) -> QJsonObject { return QJsonObject{}; });

        const auto r = src.fetch(AccountRef{"crypto:coinbase", "Coinbase", "USD"});
        QVERIFY(!r.ok);
        QVERIFY(r.holdings.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstCryptoAccountSource)
#include "tst_crypto_account_source.moc"
