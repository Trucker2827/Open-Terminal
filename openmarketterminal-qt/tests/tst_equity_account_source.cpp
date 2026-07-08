// tst_equity_account_source.cpp — Portfolio Account Sync, Task 6:
// EquityAccountSource::fetch() over an injected FakeBroker (via the
// broker-resolver callback, so no real AccountManager/BrokerRegistry/DB
// bring-up is needed). Covers:
//   1. Success — 2 BrokerHoldings + BrokerFunds.available_balance=2500 map to
//      3 SyncedHoldings: the 2 holdings (has_cost_basis=true, avg_cost=avg_price)
//      + one "$CASH:USD" cash line (has_cost_basis=false, quantity=available_balance).
//   2. Failure — get_holdings returns success=false -> FetchResult{ok=false,
//      holdings empty}. fetch() must NOT partially mirror.

#include <QtTest>

#include "services/portfolio/EquityAccountSource.h"
#include "trading/BrokerInterface.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services;

namespace {

// Minimal fake IBroker: get_holdings/get_funds are the only methods this test
// exercises; every other pure virtual is stubbed to return an empty/ok
// ApiResponse (or the cheapest valid response for non-ApiResponse returns).
class FakeBroker : public trading::IBroker {
  public:
    bool holdings_should_fail = false;

    trading::BrokerId id() const override { return trading::BrokerId::Alpaca; }
    const char* name() const override { return "FakeBroker"; }
    const char* base_url() const override { return "https://fake.invalid"; }
    trading::BrokerProfile profile() const override { return trading::BrokerProfile{}; }

    trading::TokenExchangeResponse exchange_token(const QString&, const QString&, const QString&) override {
        return {true, "FAKE-TOKEN", "", "", "", ""};
    }
    trading::OrderPlaceResponse place_order(const trading::BrokerCredentials&,
                                            const trading::UnifiedOrder&) override {
        return {false, {}, "read-only fake: place_order must never be called by an account source"};
    }
    trading::ApiResponse<QJsonObject> modify_order(const trading::BrokerCredentials&, const QString&,
                                                   const QJsonObject&) override {
        return {};
    }
    trading::ApiResponse<QJsonObject> cancel_order(const trading::BrokerCredentials&, const QString&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerOrderInfo>> get_orders(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QJsonObject> get_trade_book(const trading::BrokerCredentials&) override { return {}; }
    trading::ApiResponse<QVector<trading::BrokerPosition>> get_positions(const trading::BrokerCredentials&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerHolding>> get_holdings(const trading::BrokerCredentials&) override {
        if (holdings_should_fail)
            return {false, std::nullopt, "fixture: get_holdings failed"};

        trading::BrokerHolding aapl;
        aapl.symbol = "AAPL";
        aapl.exchange = "NASDAQ";
        aapl.quantity = 10;
        aapl.avg_price = 150.0;

        trading::BrokerHolding msft;
        msft.symbol = "MSFT";
        msft.exchange = "NASDAQ";
        msft.quantity = 5;
        msft.avg_price = 300.0;

        trading::ApiResponse<QVector<trading::BrokerHolding>> resp;
        resp.success = true;
        resp.data = QVector<trading::BrokerHolding>{aapl, msft};
        return resp;
    }
    trading::ApiResponse<trading::BrokerFunds> get_funds(const trading::BrokerCredentials&) override {
        trading::BrokerFunds funds;
        funds.available_balance = 2500.0;
        trading::ApiResponse<trading::BrokerFunds> resp;
        resp.success = true;
        resp.data = funds;
        return resp;
    }
    trading::ApiResponse<QVector<trading::BrokerQuote>> get_quotes(const trading::BrokerCredentials&,
                                                                    const QVector<QString>&) override {
        return {};
    }
    trading::ApiResponse<QVector<trading::BrokerCandle>> get_history(const trading::BrokerCredentials&,
                                                                      const QString&, const QString&, const QString&,
                                                                      const QString&) override {
        return {};
    }

  protected:
    QMap<QString, QString> auth_headers(const trading::BrokerCredentials&) const override { return {}; }
};

const portfolio::SyncedHolding* find_hold(const QVector<portfolio::SyncedHolding>& holdings, const QString& symbol) {
    for (const auto& h : holdings)
        if (h.canonical_symbol == symbol)
            return &h;
    return nullptr;
}

} // namespace

class TstEquityAccountSource : public QObject {
    Q_OBJECT

  private slots:
    void fetch_maps_holdings_and_cash() {
        FakeBroker fake;
        EquityAccountSource src([&](const QString&, trading::BrokerCredentials&) -> trading::IBroker* {
            return &fake;
        });

        const auto r = src.fetch(AccountRef{"broker:acct1", "Alpaca", "USD"});
        QVERIFY(r.ok);
        QCOMPARE(r.holdings.size(), qsizetype{3});

        const auto* cash = find_hold(r.holdings, "$CASH:USD");
        QVERIFY(cash);
        QCOMPARE(cash->quantity, 2500.0);
        QVERIFY(!cash->has_cost_basis);
        QCOMPARE(cash->avg_cost, 1.0);
        QCOMPARE(cash->native_currency, QStringLiteral("USD"));

        const auto* aapl = find_hold(r.holdings, "AAPL");
        QVERIFY(aapl);
        QVERIFY(aapl->has_cost_basis);
        QCOMPARE(aapl->avg_cost, 150.0);
        QCOMPARE(aapl->quantity, 10.0);
        QCOMPARE(aapl->broker_symbol, QStringLiteral("AAPL"));
        QCOMPARE(aapl->exchange, QStringLiteral("NASDAQ"));
        QCOMPARE(aapl->native_currency, QStringLiteral("USD"));

        const auto* msft = find_hold(r.holdings, "MSFT");
        QVERIFY(msft);
        QCOMPARE(msft->avg_cost, 300.0);
    }

    void fetch_fails_without_partial_mirror_when_get_holdings_errors() {
        FakeBroker fake;
        fake.holdings_should_fail = true;
        EquityAccountSource src([&](const QString&, trading::BrokerCredentials&) -> trading::IBroker* {
            return &fake;
        });

        const auto r = src.fetch(AccountRef{"broker:acct1", "Alpaca", "USD"});
        QVERIFY(!r.ok);
        QVERIFY(r.holdings.isEmpty());
        QVERIFY(!r.error.isEmpty());
    }

    void fetch_fails_when_resolver_cannot_find_broker() {
        EquityAccountSource src([](const QString&, trading::BrokerCredentials&) -> trading::IBroker* {
            return nullptr;
        });

        const auto r = src.fetch(AccountRef{"broker:unknown", "Ghost", "USD"});
        QVERIFY(!r.ok);
        QVERIFY(r.holdings.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstEquityAccountSource)
#include "tst_equity_account_source.moc"
