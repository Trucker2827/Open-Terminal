// tst_feed_reconnect.cpp — pure backoff + rate-limit-detection decisions for
// crypto feed auto-reconnection. No sockets, no timers, no wall-clock.
#include "services/crypto_latency/FeedReconnect.h"
#include "services/crypto_latency/CryptoLatencyService.h"

#include <QJsonObject>
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

    void reconnect_jitter_is_stable_and_symbol_specific() {
        const int btc = reconnect_jitter_ms(QStringLiteral("BTC-USD"), QStringLiteral("kraken"), true);
        const int eth = reconnect_jitter_ms(QStringLiteral("ETH-USD"), QStringLiteral("kraken"), true);
        QCOMPARE(btc, reconnect_jitter_ms(QStringLiteral("BTC-USD"), QStringLiteral("kraken"), true));
        QVERIFY(btc >= 0 && btc < 5000);
        QVERIFY(eth >= 0 && eth < 5000);
        QVERIFY(btc != eth);
        const int normal = reconnect_jitter_ms(QStringLiteral("BTC-USD"), QStringLiteral("kraken"), false);
        QVERIFY(normal >= 0 && normal < 1000);
    }

    void source_json_includes_reconnect_telemetry() {
        CryptoLatencySourceState s;
        s.source = QStringLiteral("kraken");
        s.status = QStringLiteral("disconnected");
        s.error = QStringLiteral("429 Too Many Requests");
        s.reconnect_attempts = 4;
        s.last_close_code = 1000;
        s.reconnect_delay_ms = 15000;
        s.rate_limited = true;
        s.last_tick_ms = 123;
        const QJsonObject o = CryptoLatencyService::source_to_json(s);
        QCOMPARE(o.value("source").toString(), QStringLiteral("kraken"));
        QCOMPARE(o.value("status").toString(), QStringLiteral("disconnected"));
        QCOMPARE(o.value("reconnect_attempts").toInt(), 4);
        QCOMPARE(o.value("last_close_code").toInt(), 1000);
        QCOMPARE(o.value("reconnect_delay_ms").toInt(), 15000);
        QCOMPARE(o.value("rate_limited").toBool(), true);
    }

    void filtered_snapshot_isolates_consumer_sources() {
        CryptoLatencySnapshot snapshot;
        snapshot.symbol = QStringLiteral("BTC-USD");
        CryptoLatencySourceState coinbase;
        coinbase.source = QStringLiteral("coinbase");
        coinbase.last_tick_ms = 1000;
        CryptoLatencySourceState kraken;
        kraken.source = QStringLiteral("kraken");
        kraken.last_tick_ms = 1001;
        snapshot.sources = {coinbase, kraken};

        CryptoLatencyTick cb_tick;
        cb_tick.source = QStringLiteral("coinbase");
        cb_tick.symbol = snapshot.symbol;
        cb_tick.price = 100.0;
        cb_tick.received_ts_ms = 1000;
        CryptoLatencyTick kraken_tick = cb_tick;
        kraken_tick.source = QStringLiteral("kraken");
        kraken_tick.price = 110.0;
        kraken_tick.received_ts_ms = 1001;
        snapshot.latest_ticks = {kraken_tick, cb_tick};

        const auto filtered = CryptoLatencyService::filtered_snapshot(
            snapshot, {QStringLiteral("coinbase")});
        QCOMPARE(filtered.sources.size(), 1);
        QCOMPARE(filtered.sources.first().source, QStringLiteral("coinbase"));
        QCOMPARE(filtered.latest_ticks.size(), 1);
        QCOMPARE(filtered.latest_ticks.first().source, QStringLiteral("coinbase"));
        QCOMPARE(filtered.live_sources, 1);
        QCOMPARE(filtered.min_price, 100.0);
        QCOMPARE(filtered.max_price, 100.0);
        QCOMPARE(filtered.cross_source_spread_bps, 0.0);
    }
};

QTEST_GUILESS_MAIN(TstFeedReconnect)
#include "tst_feed_reconnect.moc"
