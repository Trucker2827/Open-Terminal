#include "screens/crypto_trading/CryptoAlertEngine.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

namespace {
CryptoAlert make(const char* id, const char* kind, double threshold) {
    CryptoAlert a;
    a.id = id;
    a.exchange = "coinbase";
    a.symbol = "BTC/USD";
    a.kind = kind;
    a.threshold = threshold;
    return a;
}
} // namespace

class TstCryptoAlertEngine : public QObject {
    Q_OBJECT
  private slots:
    void cross_up_fires_once_and_needs_prior_side() {
        CryptoAlertEngine e;
        e.set_alerts({make("a1", "price_cross_up", 100.0)});
        // First observation only seeds — never fires, even at/above threshold.
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 105.0, 0, 0).isEmpty());
        // Re-seed below, then cross → fires exactly once.
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 95.0, 0, 0).isEmpty());
        const auto fired = e.on_tick("coinbase", "BTC/USD", 101.0, 0, 0);
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].id, QString("a1"));
        // Disarmed: a second qualifying cross is silent.
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 95.0, 0, 0).isEmpty());
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 102.0, 0, 0).isEmpty());
        // Re-arm restores.
        e.rearm("a1");
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 95.0, 0, 0).isEmpty());
        QCOMPARE(e.on_tick("coinbase", "BTC/USD", 103.0, 0, 0).size(), 1);
    }
    void cross_down_mirrors() {
        CryptoAlertEngine e;
        e.set_alerts({make("a2", "price_cross_down", 100.0)});
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 105.0, 0, 0).isEmpty()); // seed above
        QCOMPARE(e.on_tick("coinbase", "BTC/USD", 99.0, 0, 0).size(), 1);
    }
    void spread_alert() {
        CryptoAlertEngine e;
        e.set_alerts({make("a3", "spread_bps", 50.0)}); // 50 bps
        // spread 100.0-100.1 on mid ~100.05 → ~10 bps → silent
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 100.05, 100.0, 100.1).isEmpty());
        // spread 99-101 on mid 100 → 200 bps → fires
        QCOMPARE(e.on_tick("coinbase", "BTC/USD", 100.0, 99.0, 101.0).size(), 1);
        // disarmed afterward
        QVERIFY(e.on_tick("coinbase", "BTC/USD", 100.0, 99.0, 101.0).isEmpty());
    }
    void wrong_scope_never_fires() {
        CryptoAlertEngine e;
        e.set_alerts({make("a4", "price_cross_up", 100.0)});
        e.on_tick("coinbase", "BTC/USD", 95.0, 0, 0);
        QVERIFY(e.on_tick("kraken", "BTC/USD", 101.0, 0, 0).isEmpty());   // other exchange
        QVERIFY(e.on_tick("coinbase", "ETH/USD", 101.0, 0, 0).isEmpty()); // other symbol
    }
    void json_round_trip() {
        CryptoAlert a = make("a5", "price_cross_down", 42500.5);
        a.armed = false;
        const CryptoAlert b = CryptoAlert::from_json(a.to_json());
        QCOMPARE(b.id, a.id);
        QCOMPARE(b.exchange, a.exchange);
        QCOMPARE(b.symbol, a.symbol);
        QCOMPARE(b.kind, a.kind);
        QCOMPARE(b.threshold, a.threshold);
        QCOMPARE(b.armed, false);
    }
    void fired_alert_stays_disarmed_in_snapshot() {
        CryptoAlertEngine e;
        e.set_alerts({make("a6", "price_cross_up", 100.0)});
        e.on_tick("coinbase", "BTC/USD", 95.0, 0, 0);
        e.on_tick("coinbase", "BTC/USD", 101.0, 0, 0);
        const auto snap = e.alerts();
        QCOMPARE(snap.size(), 1);
        QVERIFY(!snap[0].armed); // persisting this prevents refire on restart
    }
};

QTEST_MAIN(TstCryptoAlertEngine)
#include "tst_crypto_alert_engine.moc"
