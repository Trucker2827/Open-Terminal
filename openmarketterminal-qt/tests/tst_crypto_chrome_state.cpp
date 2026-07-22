#include "screens/crypto_trading/CryptoChromeState.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

class TstCryptoChromeState : public QObject {
    Q_OBJECT
  private slots:
    void api_states() {
        QCOMPARE(chrome_api_state(false, -1), QString("none"));  // no creds → neutral
        QCOMPARE(chrome_api_state(false, 1), QString("none"));   // creds gone → neutral wins
        QCOMPARE(chrome_api_state(true, -1), QString("none"));   // creds but never probed
        QCOMPARE(chrome_api_state(true, 1), QString("ok"));
        QCOMPARE(chrome_api_state(true, 0), QString("error"));
    }
    void daemon_states() {
        QCOMPARE(chrome_daemon_state(false, false), QString("dead"));
        QCOMPARE(chrome_daemon_state(false, true), QString("dead")); // dead daemon wins over stale ws flag
        QCOMPARE(chrome_daemon_state(true, false), QString("rest"));
        QCOMPARE(chrome_daemon_state(true, true), QString("live"));
    }
};

QTEST_MAIN(TstCryptoChromeState)
#include "tst_crypto_chrome_state.moc"
