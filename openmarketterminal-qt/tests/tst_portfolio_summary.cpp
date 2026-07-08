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

PortfolioAsset asset(const QString& sym, double qty, double avg, const QString& sector = {},
                     bool has_cost_basis = true) {
    PortfolioAsset a;
    a.symbol = sym;
    a.quantity = qty;
    a.avg_buy_price = avg;
    a.sector = sector;
    a.has_cost_basis = has_cost_basis;
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
    void foreignHoldingConvertsToBase();
    void unresolvedFxRateBlocksSnapshot();
    void noRateLookupMeansNoConversion();
    void noCostBasisExcludedFromPnlButCountsNav();
};

// A holding whose symbol is absent from the quote map must be flagged unpriced,
// priced off avg buy price (zero P&L), and tallied in unpriced_count.
void TestPortfolioSummary::unpricedHoldingIsFlaggedAndCounted() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("MSFT", 5, 200.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 5.0, 3.45)); // MSFT deliberately missing

    const auto built = build_summary(assets, {}, quotes, stub_sector);

    QCOMPARE(built.unpriced_count, 1);
    QVERIFY(!built.snapshot_safe()); // an unpriced holding must block the snapshot
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
    QVERIFY(built.snapshot_safe()); // all priced + no FX conversion -> safe to snapshot
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

// A foreign holding is reported entirely in the portfolio's base currency: every
// value (avg cost, price, day change, market value, P&L) is multiplied by the
// native->base FX rate. day_change_percent is a ratio and stays as-is.
void TestPortfolioSummary::foreignHoldingConvertsToBase() {
    const QVector<PortfolioAsset> assets{asset("RY.TO", 10, 100.0)}; // avg 100 CAD
    QHash<QString, QuoteData> quotes;
    quotes.insert("RY.TO", quote("RY.TO", 150.0, 10.0, 7.14)); // 150 CAD, +10 CAD

    // CAD -> USD at 0.5 (clean numbers).
    const auto rate = [](const QString&) { return 0.5; };
    const auto built = build_summary(assets, {}, quotes, stub_sector, rate);

    QCOMPARE(built.fx_unresolved_count, 0);
    QVERIFY(built.snapshot_safe());
    const auto& h = built.summary.holdings[0];
    QVERIFY(h.fx_resolved);
    QCOMPARE(h.avg_buy_price, 50.0);        // 100 * 0.5
    QCOMPARE(h.cost_basis, 500.0);          // 10 * 50
    QCOMPARE(h.current_price, 75.0);        // 150 * 0.5
    QCOMPARE(h.day_change, 5.0);            // 10 * 0.5
    QCOMPARE(h.day_change_percent, 7.14);   // ratio unchanged
    QCOMPARE(h.market_value, 750.0);        // 10 * 75
    QCOMPARE(h.unrealized_pnl, 250.0);      // 750 - 500
    QCOMPARE(h.unrealized_pnl_percent, 50.0);
    QCOMPARE(built.summary.total_market_value, 750.0);
}

// An unresolved FX rate (rate_lookup returns <= 0) converts at 1.0 for display
// but MUST block the snapshot — otherwise a guessed base value contaminates NAV
// history exactly like a fabricated price does.
void TestPortfolioSummary::unresolvedFxRateBlocksSnapshot() {
    const QVector<PortfolioAsset> assets{asset("RY.TO", 10, 100.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("RY.TO", quote("RY.TO", 150.0, 0.0, 0.0));

    const auto unresolved = [](const QString&) { return 0.0; }; // rate not known yet
    const auto built = build_summary(assets, {}, quotes, stub_sector, unresolved);

    QCOMPARE(built.fx_unresolved_count, 1);
    QVERIFY(!built.snapshot_safe());
    QVERIFY(!built.summary.holdings[0].fx_resolved); // flagged for the display badge
    // Converted at 1.0 for display (unconverted), not zeroed.
    QCOMPARE(built.summary.holdings[0].current_price, 150.0);
    QCOMPARE(built.summary.total_market_value, 1500.0);
}

// An absent rate_lookup (the broker path / legacy callers) means no conversion:
// every rate is 1.0, nothing is fx-unresolved, and the snapshot proceeds.
void TestPortfolioSummary::noRateLookupMeansNoConversion() {
    const QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0)};
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 5.0, 3.45));

    const auto built = build_summary(assets, {}, quotes, stub_sector); // no rate_lookup

    QCOMPARE(built.fx_unresolved_count, 0);
    QVERIFY(built.snapshot_safe());
    QCOMPARE(built.summary.holdings[0].current_price, 150.0); // unchanged
    QCOMPARE(built.summary.total_market_value, 1500.0);
}

// A holding with no cost basis (e.g. a crypto exchange balance where only
// quantity is known) must still count toward NAV via market_value, but must
// be excluded from cost basis / unrealized P&L totals — folding its market
// value into P&L would fabricate a gain/loss with no real cost to compare
// against.
void TestPortfolioSummary::noCostBasisExcludedFromPnlButCountsNav() {
    QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("BTC-USD", 1, 0.0)};
    assets[1].has_cost_basis = false; // crypto: qty known, cost basis not
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 0.0, 0.0));
    quotes.insert("BTC-USD", quote("BTC-USD", 60000.0, 0.0, 0.0));
    const auto built = build_summary(assets, {}, quotes, stub_sector);
    // NAV includes both: 10*150 + 1*60000 = 61500
    QCOMPARE(built.summary.total_market_value, 61500.0);
    // Cost basis / P&L exclude the no-cost-basis holding: only AAPL (cost 1000).
    QCOMPARE(built.summary.total_cost_basis, 1000.0);
    QCOMPARE(built.summary.total_unrealized_pnl, 500.0); // 1500 - 1000
    // The crypto holding carries the flag for the display badge.
    QVERIFY(!built.summary.holdings[1].has_cost_basis);
    QVERIFY(built.snapshot_safe()); // priced + fx-resolved -> still snapshots
}

QTEST_APPLESS_MAIN(TestPortfolioSummary)
#include "tst_portfolio_summary.moc"
