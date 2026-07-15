// tst_feed_reconnect.cpp — pure backoff + rate-limit-detection decisions for
// crypto feed auto-reconnection. No sockets, no timers, no wall-clock.
#include "services/crypto_latency/FeedReconnect.h"

#include <QtTest/QtTest>

using namespace openmarketterminal::services::crypto_latency;

class TstFeedReconnect : public QObject {
    Q_OBJECT
  private slots:
    void is_rate_limited_detects_429() {
        QVERIFY(is_rate_limited(QStringLiteral(
            "QWebSocketPrivate::processHandshake: Unhandled http status code: 429 (Too Many Requests).")));
        QVERIFY(is_rate_limited(QStringLiteral("429")));
        QVERIFY(is_rate_limited(QStringLiteral("too many requests")));
        QVERIFY(!is_rate_limited(QStringLiteral("Connection refused")));
        QVERIFY(!is_rate_limited(QString()));
    }

    void backoff_grows_and_caps() {
        // base 1000, cap 60000, floor 15000
        QCOMPARE(next_reconnect_delay_ms(0, false, 1000, 60000, 15000), 1000);  // 1000<<0
        QCOMPARE(next_reconnect_delay_ms(1, false, 1000, 60000, 15000), 2000);  // 1000<<1
        QCOMPARE(next_reconnect_delay_ms(3, false, 1000, 60000, 15000), 8000);  // 1000<<3
        QCOMPARE(next_reconnect_delay_ms(20, false, 1000, 60000, 15000), 60000); // capped
        QCOMPARE(next_reconnect_delay_ms(6, false, 1000, 60000, 15000), 60000);  // 64000->cap
    }

    void rate_limited_uses_higher_floor() {
        // A rate-limited reconnect never waits less than the floor, even on attempt 0.
        QCOMPARE(next_reconnect_delay_ms(0, true, 1000, 60000, 15000), 15000);  // max(15000,1000)
        QCOMPARE(next_reconnect_delay_ms(1, true, 1000, 60000, 15000), 15000);  // max(15000,2000)
        QCOMPARE(next_reconnect_delay_ms(5, true, 1000, 60000, 15000), 32000);  // max(15000,32000)
        QCOMPARE(next_reconnect_delay_ms(20, true, 1000, 60000, 15000), 60000); // capped
    }
};

QTEST_GUILESS_MAIN(TstFeedReconnect)
#include "tst_feed_reconnect.moc"
