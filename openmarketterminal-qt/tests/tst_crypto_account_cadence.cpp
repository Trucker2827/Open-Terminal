#include "screens/crypto_trading/CryptoAccountCadence.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

class TstCryptoAccountCadence : public QObject {
    Q_OBJECT
  private slots:
    void baseline_when_no_ws() { QCOMPARE(account_poll_interval_ms(1'000'000, 0), 5000); }
    void baseline_when_negative_stamp() { QCOMPARE(account_poll_interval_ms(1'000'000, -5), 5000); }
    void relaxed_when_ws_fresh() { QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 9'000), 30000); }
    void snaps_back_on_staleness() { QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 16'000), 5000); }
    void boundary_is_fifteen_seconds() {
        QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 15'000), 5000);  // >= 15s stale → baseline
        QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 14'999), 30000); // just inside the window
    }
};

QTEST_MAIN(TstCryptoAccountCadence)
#include "tst_crypto_account_cadence.moc"
