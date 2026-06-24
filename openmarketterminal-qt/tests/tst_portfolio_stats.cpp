// Known-answer coverage for the portfolio risk-statistics helpers
// (services/portfolio/PortfolioStats) extracted from
// PortfolioService::compute_metrics: annualised Sharpe / Sortino and max
// drawdown. These feed the portfolio analytics the user reads; a silent error
// reports a fictitious risk/return profile. Reference values are computed by
// hand from the documented formulas.

#include "services/portfolio/PortfolioStats.h"

#include <QtTest>

using namespace openmarketterminal::portfolio::stats;

class TestPortfolioStats : public QObject {
    Q_OBJECT
  private slots:
    void sharpeKnownAnswer();
    void sharpeSubtractsRiskFree();
    void sharpeDegenerateCasesAreZero();
    void sortinoKnownAnswer();
    void sortinoNoDownsideIsZero();
    void maxDrawdownKnownAnswer();
    void maxDrawdownEdgeCases();
};

// Daily return series (%) used across several cases: mean 0.2, sample sd
// 0.9082951, two negative days.
static const QVector<double> kReturns{1.0, -0.5, 0.5, 1.0, -1.0};

void TestPortfolioStats::sharpeKnownAnswer() {
    // (0.2 / 0.9082951) * sqrt(252) = 3.495452
    const auto s = sharpe_annualized(kReturns, 0.0, 252.0);
    QVERIFY(s.has_value());
    QVERIFY(std::abs(*s - 3.495452) < 1e-5);
}

void TestPortfolioStats::sharpeSubtractsRiskFree() {
    // rf 0.1%/period: ((0.2 - 0.1) / 0.9082951) * sqrt(252) = 1.747726
    const auto s = sharpe_annualized(kReturns, 0.1, 252.0);
    QVERIFY(s.has_value());
    QVERIFY(std::abs(*s - 1.747726) < 1e-5);
}

void TestPortfolioStats::sharpeDegenerateCasesAreZero() {
    // Undefined cases return nullopt so the caller leaves the metric unset.
    QVERIFY(!sharpe_annualized({}, 0.0, 252.0).has_value());              // empty
    QVERIFY(!sharpe_annualized({1.5}, 0.0, 252.0).has_value());          // single point
    QVERIFY(!sharpe_annualized({0.5, 0.5, 0.5}, 0.0, 252.0).has_value());  // zero variance
}

void TestPortfolioStats::sortinoKnownAnswer() {
    // downside dev = sqrt(((-0.5)^2 + (-1)^2)/5) = 0.5; (0.2/0.5)*sqrt(252) = 6.349803
    const auto s = sortino_annualized(kReturns, 0.0, 252.0);
    QVERIFY(s.has_value());
    QVERIFY(std::abs(*s - 6.349803) < 1e-5);
}

void TestPortfolioStats::sortinoNoDownsideIsZero() {
    // All returns at/above the risk-free rate -> no downside -> nullopt.
    QVERIFY(!sortino_annualized({1.0, 2.0, 3.0}, 0.0, 252.0).has_value());
    QVERIFY(!sortino_annualized({}, 0.0, 252.0).has_value());
}

void TestPortfolioStats::maxDrawdownKnownAnswer() {
    // peak hits 120 then drops to 60 -> (60-120)/120*100 = -50%.
    QCOMPARE(max_drawdown_pct({100, 110, 90, 95, 120, 60}), -50.0);
}

void TestPortfolioStats::maxDrawdownEdgeCases() {
    QCOMPARE(max_drawdown_pct({}), 0.0);                  // empty
    QCOMPARE(max_drawdown_pct({100, 110, 120}), 0.0);    // monotonic up -> no drawdown
    QCOMPARE(max_drawdown_pct({100}), 0.0);              // single point
}

QTEST_APPLESS_MAIN(TestPortfolioStats)
#include "tst_portfolio_stats.moc"
