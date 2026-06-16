// tst_crypto_risk.cpp — pure crypto order-notional risk verdict.
#include <QtTest>
#include <limits>
#include "trading/CryptoRisk.h"

using namespace openmarketterminal::trading;

class TstCryptoRisk : public QObject {
    Q_OBJECT
  private slots:
    void order_value_is_qty_times_price() {
        QCOMPARE(crypto_risk_verdict(0.001, 60000.0, 500.0).order_value, 60.0);
        QCOMPARE(crypto_risk_verdict(2.0, 1.21, 500.0).order_value, 2.42);
    }
    void under_cap_passes() {
        const auto v = crypto_risk_verdict(0.001, 60000.0, 500.0); // $60 <= $500
        QVERIFY(v.ok);
        QVERIFY(v.reason.isEmpty());
    }
    void over_cap_rejected_with_reason() {
        const auto v = crypto_risk_verdict(0.02, 60000.0, 500.0);   // $1200 > $500
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains("max order value"));
    }
    void zero_price_rejected() {
        const auto v = crypto_risk_verdict(1.0, 0.0, 500.0);
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains("no price"));
    }
    void zero_or_negative_qty_rejected() {
        QVERIFY(!crypto_risk_verdict(0.0, 60000.0, 500.0).ok);
        QVERIFY(!crypto_risk_verdict(-1.0, 60000.0, 500.0).ok);
    }
    void non_finite_price_rejected() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        QVERIFY(!crypto_risk_verdict(1.0, nan, 500.0).ok);
        QVERIFY(!crypto_risk_verdict(1.0, inf, 500.0).ok);   // inf price → not finite → rejected
        QVERIFY(!crypto_risk_verdict(nan, 60000.0, 500.0).ok);
    }
    void at_cap_boundary_passes() {
        const auto v = crypto_risk_verdict(1.0, 500.0, 500.0); // exactly at cap, strict > → passes
        QVERIFY(v.ok);
    }
};
QTEST_MAIN(TstCryptoRisk)
#include "tst_crypto_risk.moc"
