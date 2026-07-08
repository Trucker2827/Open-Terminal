// Coverage for the pure portfolio-summary builder
// (services/portfolio/PortfolioSummaryBuild) extracted from
// PortfolioService::finalize_summary. The behaviour under test is the fix for
// the "unpriced holding" bug: a holding with no live quote must be flagged
// (priced=false), fall back to its avg buy price, and be counted in
// unpriced_count so finalize_summary refuses to persist a NAV snapshot built on
// a fabricated mark. A silent fallback would (a) look like a real flat position
// and (b) permanently contaminate NAV history.

#include "services/portfolio/PortfolioSummaryBuild.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;
using openmarketterminal::services::QuoteData;

namespace {

PortfolioAsset asset(const QString& sym, double qty, double avg, const QString& sector = {}) {
    PortfolioAsset a;
    a.symbol = sym;
    a.quantity = qty;
    a.avg_buy_price = avg;
    a.sector = sector;
    return a;
}

QuoteData quote(const QString& sym, double price, double change, double change_pct) {
    QuoteData q;
    q.symbol = sym;
    q.price = price;
    q.change = change;
    q.change_pct = change_pct;
    return q;
}

// Identity sector lookup so tests can assert the fallback path was taken.
QString stub_sector(const QString& sym) { return "SECTOR:" + sym; }

} // namespace

class TestPortfolioSummary : public QObject {
    Q_OBJECT
  private slots:
    void unpricedHoldingIsFlaggedAndCounted();
    void fullyPricedHasZeroUnpriced();
    void unpricedMarkStillContaminatesTotal();
    void sectorFallbackUsedOnlyWhenEmpty();
};

// A holding whose symbol is absent from the quote map must be flagged unpriced,
// priced off avg buy price (zero P&L), and tallied in unpriced_count.
void TestPortfolioSummary::unpricedHoldingIsFlaggedAndCounted() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("MSFT", 5, 200.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 5.0, 3.45)); // MSFT deliberately missing

    const auto built = build_summary(assets, {}, quotes, stub_sector);

    QCOMPARE(built.unpriced_count, 1);
    QCOMPARE(built.summary.holdings.size(), qsizetype{2});

    const auto& aapl = built.summary.holdings[0];
    QVERIFY(aapl.priced);
    QCOMPARE(aapl.current_price, 150.0);
    QCOMPARE(aapl.market_value, 1500.0);

    const auto& msft = built.summary.holdings[1];
    QVERIFY(!msft.priced);
    QCOMPARE(msft.current_price, 200.0);       // fell back to avg buy price
    QCOMPARE(msft.market_value, msft.cost_basis); // fabricated -> zero P&L
    QCOMPARE(msft.unrealized_pnl, 0.0);
    QCOMPARE(msft.day_change, 0.0);
}

// Every holding priced -> unpriced_count 0 -> finalize_summary will snapshot.
void TestPortfolioSummary::fullyPricedHasZeroUnpriced() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("MSFT", 5, 200.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 5.0, 3.45));
    quotes.insert("MSFT", quote("MSFT", 210.0, -2.0, -0.94));

    const auto built = build_summary(assets, {}, quotes, stub_sector);

    QCOMPARE(built.unpriced_count, 0);
    QVERIFY(built.summary.holdings[0].priced);
    QVERIFY(built.summary.holdings[1].priced);
    // 10*150 + 5*210 = 2550
    QCOMPARE(built.summary.total_market_value, 2550.0);
}

// The fabricated mark of an unpriced holding still flows into the portfolio
// total — which is exactly why finalize_summary must skip the snapshot. This
// documents the contamination the unpriced_count gate prevents.
void TestPortfolioSummary::unpricedMarkStillContaminatesTotal() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("GHOST", 4, 50.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 0.0, 0.0));

    const auto built = build_summary(assets, {}, quotes, stub_sector);

    QCOMPARE(built.unpriced_count, 1);
    // 10*150 (real) + 4*50 (fabricated) = 1700 — the 200 is not a real mark.
    QCOMPARE(built.summary.total_market_value, 1700.0);
}

// Stored sector is kept; only an empty sector triggers the lookup callback.
void TestPortfolioSummary::sectorFallbackUsedOnlyWhenEmpty() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 1, 100.0, "Technology"),
                                         asset("XOM", 1, 100.0 /* empty sector */)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 100.0, 0.0, 0.0));
    quotes.insert("XOM", quote("XOM", 100.0, 0.0, 0.0));

    const auto built = build_summary(assets, {}, quotes, stub_sector);

    QCOMPARE(built.summary.holdings[0].sector, QString("Technology"));
    QCOMPARE(built.summary.holdings[1].sector, QString("SECTOR:XOM"));
}

QTEST_APPLESS_MAIN(TestPortfolioSummary)
#include "tst_portfolio_summary.moc"
