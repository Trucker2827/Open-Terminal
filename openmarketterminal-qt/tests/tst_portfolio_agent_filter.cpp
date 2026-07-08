// Coverage for the pure Agent Runner relevance filter
// (screens/portfolio/PortfolioAgentFilter): classify holdings by asset class and
// decide which agents (equity / fx / crypto / general) are relevant.

#include "screens/portfolio/PortfolioAgentFilter.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;

class TestPortfolioAgentFilter : public QObject {
    Q_OBJECT
  private slots:
    void classifiesSymbols();
    void portfolioClassesFromSymbols();
    void generalAgentAlwaysShows();
    void assetSpecificAgentFilters();
    void emptyOrUntaggedShowsAll();
};

void TestPortfolioAgentFilter::classifiesSymbols() {
    QCOMPARE(symbol_asset_class("BTC-USD"), QString("crypto"));
    QCOMPARE(symbol_asset_class("ETH-USD"), QString("crypto"));
    QCOMPARE(symbol_asset_class("AAPL"), QString("equity"));
    QCOMPARE(symbol_asset_class("EURUSD=X"), QString("fx"));
    QCOMPARE(symbol_asset_class("$CASH:USD"), QString("cash"));
}

void TestPortfolioAgentFilter::portfolioClassesFromSymbols() {
    // A Coinbase book: crypto coins + cash -> {crypto} (cash ignored).
    const auto c = portfolio_asset_classes({"BTC-USD", "ETH-USD", "$CASH:USD"});
    QCOMPARE(c.size(), 1);
    QVERIFY(c.contains("crypto"));
    QVERIFY(!c.contains("equity"));
    // Mixed (All Accounts) -> both.
    const auto m = portfolio_asset_classes({"BTC-USD", "AAPL"});
    QVERIFY(m.contains("crypto"));
    QVERIFY(m.contains("equity"));
}

void TestPortfolioAgentFilter::generalAgentAlwaysShows() {
    const QSet<QString> crypto{"crypto"};
    QVERIFY(agent_matches_holdings({"*"}, crypto));       // general
    QVERIFY(agent_matches_holdings({"crypto"}, crypto));  // exact match
}

void TestPortfolioAgentFilter::assetSpecificAgentFilters() {
    const QSet<QString> crypto{"crypto"};
    // An equity value-investor / FX agent is hidden for a crypto-only book.
    QVERIFY(!agent_matches_holdings({"equity"}, crypto));
    QVERIFY(!agent_matches_holdings({"fx"}, crypto));
    // ...but shown for an equity book.
    const QSet<QString> equity{"equity"};
    QVERIFY(agent_matches_holdings({"equity"}, equity));
    QVERIFY(!agent_matches_holdings({"fx"}, equity));
}

void TestPortfolioAgentFilter::emptyOrUntaggedShowsAll() {
    // Unknown holdings (cash-only / empty) -> never filter to nothing.
    QVERIFY(agent_matches_holdings({"equity"}, {}));
    // Untagged agent -> always show.
    QVERIFY(agent_matches_holdings({}, QSet<QString>{"crypto"}));
}

QTEST_APPLESS_MAIN(TestPortfolioAgentFilter)
#include "tst_portfolio_agent_filter.moc"
