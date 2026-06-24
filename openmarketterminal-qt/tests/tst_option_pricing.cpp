// Known-answer + invariant coverage for the synchronous Black-Scholes /
// Black-Scholes-Merton / Black pricers (services/options/OptionPricing). These
// drive the strategy payoff curve at hundreds of spot points per scrub frame,
// so a silent error here mis-prices every analytics ribbon. Reference values
// are the canonical textbook BS example and closed-form invariants that hold
// independently of the implementation.

#include "services/options/OptionPricing.h"

#include <QtTest>

#include <cmath>

using namespace openmarketterminal::services::options::pricing;

namespace {
// Discount factor e^(-rt).
double disc(double r, double t) { return std::exp(-r * t); }
}  // namespace

class TestOptionPricing : public QObject {
    Q_OBJECT
  private slots:
    void normalCdfKnownValues();
    void atmCallPutKnownAnswer();
    void putCallParityHolds();
    void expiryReturnsIntrinsic();
    void zeroVolReturnsDiscountedForwardIntrinsic();
    void nonPositiveInputsReturnZero();
    void blackAtForwardEqualsBlackScholes();
    void callIncreasesWithVolatility();
};

void TestOptionPricing::normalCdfKnownValues() {
    QVERIFY(std::abs(normal_cdf(0.0) - 0.5) < 1e-12);
    QVERIFY(std::abs(normal_cdf(1.959964) - 0.975) < 1e-5);   // 97.5th percentile
    QVERIFY(std::abs(normal_cdf(-1.959964) - 0.025) < 1e-5);
    // Symmetry: N(x) + N(-x) == 1 for any x.
    for (double x : {-2.3, -0.7, 0.4, 1.1, 3.0})
        QVERIFY(std::abs((normal_cdf(x) + normal_cdf(-x)) - 1.0) < 1e-12);
}

void TestOptionPricing::atmCallPutKnownAnswer() {
    // The canonical BS example: S=K=100, t=1y, r=5%, sigma=20%, no dividend.
    // d1=0.35, d2=0.15 → call ≈ 10.45058, put ≈ 5.57353.
    const double call = bs_call(100, 100, 1.0, 0.05, 0.20);
    const double put = bs_put(100, 100, 1.0, 0.05, 0.20);
    QVERIFY2(std::abs(call - 10.450584) < 1e-4, qPrintable(QString::number(call, 'f', 6)));
    QVERIFY2(std::abs(put - 5.573526) < 1e-4, qPrintable(QString::number(put, 'f', 6)));
}

void TestOptionPricing::putCallParityHolds() {
    // BSM parity: call - put == S*e^(-qt) - K*e^(-rt), for ANY valid inputs.
    struct P { double S, K, t, r, sigma, q; };
    for (const P& p : {P{100, 90, 0.5, 0.03, 0.25, 0.0}, P{80, 100, 2.0, 0.06, 0.40, 0.02},
                       P{120, 100, 0.25, 0.01, 0.15, 0.04}}) {
        const double lhs = bsm_call(p.S, p.K, p.t, p.r, p.sigma, p.q) -
                           bsm_put(p.S, p.K, p.t, p.r, p.sigma, p.q);
        const double rhs = p.S * std::exp(-p.q * p.t) - p.K * disc(p.r, p.t);
        QVERIFY2(std::abs(lhs - rhs) < 1e-9, qPrintable(QString::number(lhs - rhs, 'e', 3)));
    }
}

void TestOptionPricing::expiryReturnsIntrinsic() {
    // t <= 0 → undiscounted intrinsic value.
    QCOMPARE(bs_call(110, 100, 0.0, 0.05, 0.2), 10.0);
    QCOMPARE(bs_call(90, 100, 0.0, 0.05, 0.2), 0.0);
    QCOMPARE(bs_put(90, 100, 0.0, 0.05, 0.2), 10.0);
    QCOMPARE(bs_put(110, 100, -1.0, 0.05, 0.2), 0.0);
}

void TestOptionPricing::zeroVolReturnsDiscountedForwardIntrinsic() {
    // sigma <= 0 → deterministic discounted forward intrinsic (q=0).
    // call = max(S - K*e^(-rt), 0): 100 - 100*e^(-0.05) = 4.877058.
    QVERIFY(std::abs(bs_call(100, 100, 1.0, 0.05, 0.0) - (100.0 - 100.0 * disc(0.05, 1.0))) < 1e-9);
    // Deep OTM with zero vol → worthless.
    QCOMPARE(bs_call(50, 100, 1.0, 0.05, 0.0), 0.0);
}

void TestOptionPricing::nonPositiveInputsReturnZero() {
    QCOMPARE(bs_call(-5, 100, 1.0, 0.05, 0.2), 0.0);
    QCOMPARE(bs_call(100, 0, 1.0, 0.05, 0.2), 0.0);
    QCOMPARE(bs_put(0, 100, 1.0, 0.05, 0.2), 0.0);
}

void TestOptionPricing::blackAtForwardEqualsBlackScholes() {
    // Black(F=S*e^(rt)) must equal Black-Scholes(S) — same d1/d2, discounted forward.
    const double S = 100, K = 105, t = 0.75, r = 0.04, sigma = 0.3;
    const double F = S * std::exp(r * t);
    QVERIFY(std::abs(black_call(F, K, t, r, sigma) - bs_call(S, K, t, r, sigma)) < 1e-9);
    QVERIFY(std::abs(black_put(F, K, t, r, sigma) - bs_put(S, K, t, r, sigma)) < 1e-9);
}

void TestOptionPricing::callIncreasesWithVolatility() {
    // Vega > 0: a call is strictly increasing in sigma (all else equal).
    double prev = bs_call(100, 100, 1.0, 0.05, 0.05);
    for (double sigma : {0.10, 0.20, 0.40, 0.80}) {
        const double next = bs_call(100, 100, 1.0, 0.05, sigma);
        QVERIFY2(next > prev, qPrintable(QString("sigma=%1 price=%2 not > %3")
                                             .arg(sigma).arg(next).arg(prev)));
        prev = next;
    }
}

QTEST_APPLESS_MAIN(TestOptionPricing)
#include "tst_option_pricing.moc"
