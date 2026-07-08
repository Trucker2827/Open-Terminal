// Coverage for the pure account-sync mirror-reconcile diff
// (services/portfolio/AccountSyncTypes::reconcile_mirror). This is the core of
// "mirror the account exactly": given the portfolio's current assets and a
// freshly fetched set of holdings from a connected broker/exchange, compute
// the add/update/remove plan needed to make the portfolio match the source
// exactly — including the case where a successful fetch comes back empty
// (the account is flat), which must remove every existing holding.

#include "services/portfolio/AccountSyncTypes.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;

namespace {

PortfolioAsset mkAsset(const QString& symbol, double quantity, double avg_buy_price,
                       bool has_cost_basis = true) {
    PortfolioAsset a;
    a.symbol = symbol;
    a.quantity = quantity;
    a.avg_buy_price = avg_buy_price;
    a.has_cost_basis = has_cost_basis;
    return a;
}

SyncedHolding mkHold(const QString& canonical_symbol, double quantity, double avg_cost,
                     bool has_cost_basis = true) {
    SyncedHolding h;
    h.canonical_symbol = canonical_symbol;
    h.quantity = quantity;
    h.avg_cost = avg_cost;
    h.has_cost_basis = has_cost_basis;
    return h;
}

const PortfolioAsset* findBySymbol(const QVector<PortfolioAsset>& v, const QString& symbol) {
    for (const auto& a : v)
        if (a.symbol == symbol) return &a;
    return nullptr;
}

} // namespace

class TestAccountSyncReconcile : public QObject {
    Q_OBJECT
  private slots:
    void addsUpdatesRemoves();
    void identicalIsNoop();
    void emptyFetchRemovesAll();
    void aggregateMergesDuplicateSymbols();
    void aggregateAndsCostBasis();
};

// new fetched symbol -> add; qty change -> update; vanished -> remove; identical -> no-op.
void TestAccountSyncReconcile::addsUpdatesRemoves() {
    QVector<PortfolioAsset> current{mkAsset("AAPL", 8, 100.0), mkAsset("TSLA", 3, 200.0)};
    QVector<SyncedHolding> fetched{mkHold("AAPL", 10, 100.0), mkHold("MSFT", 5, 50.0)};
    auto plan = reconcile_mirror(current, fetched);
    QCOMPARE(plan.to_add.size(), qsizetype{1});
    QCOMPARE(plan.to_add[0].canonical_symbol, QString("MSFT"));
    QCOMPARE(plan.to_update.size(), qsizetype{1});
    QCOMPARE(plan.to_update[0].canonical_symbol, QString("AAPL"));
    QCOMPARE(plan.to_remove, QStringList{"TSLA"});
}

void TestAccountSyncReconcile::identicalIsNoop() {
    QVector<PortfolioAsset> current{mkAsset("AAPL", 10, 100.0)};
    QVector<SyncedHolding> fetched{mkHold("AAPL", 10, 100.0)};
    auto plan = reconcile_mirror(current, fetched);
    QVERIFY(plan.to_add.isEmpty());
    QVERIFY(plan.to_update.isEmpty());
    QVERIFY(plan.to_remove.isEmpty());
}

void TestAccountSyncReconcile::emptyFetchRemovesAll() { // successful empty fetch mirrors to empty
    QVector<PortfolioAsset> current{mkAsset("AAPL", 10, 100.0)};
    auto plan = reconcile_mirror(current, {});
    QCOMPARE(plan.to_remove, QStringList{"AAPL"});
}

// duplicate symbol across portfolios -> summed qty, quantity-weighted avg price
void TestAccountSyncReconcile::aggregateMergesDuplicateSymbols() {
    QVector<PortfolioAsset> alpaca{mkAsset("AAPL", 10, 100.0)};
    QVector<PortfolioAsset> ibkr{mkAsset("AAPL", 5, 130.0), mkAsset("BND", 4, 70.0)};
    auto merged = aggregate_holdings({alpaca, ibkr});
    // AAPL merged: qty 15, weighted avg = (10*100 + 5*130)/15 = 110
    auto aapl = findBySymbol(merged, "AAPL");
    QVERIFY(aapl);
    QCOMPARE(aapl->quantity, 15.0);
    QCOMPARE(aapl->avg_buy_price, 110.0);
    QCOMPARE(merged.size(), qsizetype{2}); // AAPL + BND
}

// has_cost_basis on the merged row is the AND of all contributors
void TestAccountSyncReconcile::aggregateAndsCostBasis() {
    QVector<PortfolioAsset> a{mkAsset("BTC-USD", 1, 0.0)};
    a[0].has_cost_basis = false;
    QVector<PortfolioAsset> b{mkAsset("BTC-USD", 2, 0.0)};
    b[0].has_cost_basis = false;
    auto merged = aggregate_holdings({a, b});
    QCOMPARE(merged.size(), qsizetype{1});
    QCOMPARE(merged[0].quantity, 3.0);
    QVERIFY(!merged[0].has_cost_basis);
}

QTEST_APPLESS_MAIN(TestAccountSyncReconcile)
#include "tst_account_sync_reconcile.moc"
