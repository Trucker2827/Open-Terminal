// Coverage for the pure holding-display helper
// (screens/portfolio/PortfolioHoldingDisplay) extracted from PortfolioBlotter.
// The behaviour under test is the "unpriced holding" badge: a holding with no
// live quote (priced=false) must render its price-dependent cells as em-dashes
// (muted) instead of a fabricated "+0.00%" flat position, while a priced holding
// formats exactly as the blotter did before.

#include "screens/portfolio/PortfolioHoldingDisplay.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;

namespace {
HoldingWithQuote holding(double price, double mv, double pnl, double pnl_pct, double chg_pct, bool priced,
                         bool fx_resolved = true) {
    HoldingWithQuote h;
    h.current_price = price;
    h.market_value = mv;
    h.unrealized_pnl = pnl;
    h.unrealized_pnl_percent = pnl_pct;
    h.day_change_percent = chg_pct;
    h.priced = priced;
    h.fx_resolved = fx_resolved;
    return h;
}
} // namespace

class TestPortfolioDisplay : public QObject {
    Q_OBJECT
  private slots:
    void pricedHoldingFormatsNumbers();
    void pricedHoldingSignsNegatives();
    void unpricedHoldingShowsDashes();
    void fxUnresolvedHoldingShowsDashes();
};

void TestPortfolioDisplay::pricedHoldingFormatsNumbers() {
    const auto c = price_dependent_cells(holding(150.0, 1500.0, 500.0, 50.0, 3.45, /*priced=*/true));
    QVERIFY(!c.muted);
    QCOMPARE(c.last, QString("150.00"));
    QCOMPARE(c.market_value, QString("1500.00"));
    QCOMPARE(c.pnl, QString("+500.00"));
    QCOMPARE(c.pnl_pct, QString("+50.00%"));
    QCOMPARE(c.day_change_pct, QString("+3.45%"));
}

void TestPortfolioDisplay::pricedHoldingSignsNegatives() {
    const auto c = price_dependent_cells(holding(90.0, 900.0, -100.0, -10.0, -2.0, /*priced=*/true));
    QVERIFY(!c.muted);
    QCOMPARE(c.pnl, QString("-100.00"));
    QCOMPARE(c.pnl_pct, QString("-10.00%"));
    QCOMPARE(c.day_change_pct, QString("-2.00%"));
}

// The regression guard: an unpriced holding must NOT render numeric marks (which
// would be fabricated) — every price-dependent cell is the placeholder dash and
// the row is flagged muted.
void TestPortfolioDisplay::unpricedHoldingShowsDashes() {
    // Pass the fabricated fallback values a real unpriced holding would carry
    // (market_value == cost_basis, zero P&L) to prove they are suppressed.
    const auto c = price_dependent_cells(holding(200.0, 1000.0, 0.0, 0.0, 0.0, /*priced=*/false));
    const QString dash = unpriced_cell_placeholder();
    QVERIFY(c.muted);
    QCOMPARE(c.reason, unpriced_reason());
    QCOMPARE(c.last, dash);
    QCOMPARE(c.market_value, dash);
    QCOMPARE(c.pnl, dash);
    QCOMPARE(c.pnl_pct, dash);
    QCOMPARE(c.day_change_pct, dash);
}

// A priced holding whose FX rate didn't resolve is shown at the native scale
// (wrong for the base currency), so it must also dash + mute — with a distinct
// tooltip so the user knows it's an FX gap, not a missing quote.
void TestPortfolioDisplay::fxUnresolvedHoldingShowsDashes() {
    const auto c = price_dependent_cells(
        holding(150.0, 1500.0, 500.0, 50.0, 3.45, /*priced=*/true, /*fx_resolved=*/false));
    const QString dash = unpriced_cell_placeholder();
    QVERIFY(c.muted);
    QCOMPARE(c.reason, fx_unresolved_reason());
    QCOMPARE(c.last, dash);
    QCOMPARE(c.market_value, dash);
    QCOMPARE(c.pnl, dash);
    QCOMPARE(c.pnl_pct, dash);
    QCOMPARE(c.day_change_pct, dash);
}

QTEST_APPLESS_MAIN(TestPortfolioDisplay)
#include "tst_portfolio_display.moc"
