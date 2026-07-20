// tst_feed_reconnect.cpp — pure backoff + rate-limit-detection decisions for
// crypto feed auto-reconnection. No sockets, no timers, no wall-clock.
#include "services/crypto_latency/FeedReconnect.h"
#include "services/crypto_latency/CryptoLatencyService.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
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
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        CryptoLatencySnapshot snapshot;
        snapshot.symbol = QStringLiteral("BTC-USD");
        CryptoLatencySourceState coinbase;
        coinbase.source = QStringLiteral("coinbase");
        coinbase.last_tick_ms = now;
        CryptoLatencySourceState kraken;
        kraken.source = QStringLiteral("kraken");
        kraken.last_tick_ms = now;
        snapshot.sources = {coinbase, kraken};

        CryptoLatencyTick cb_tick;
        cb_tick.source = QStringLiteral("coinbase");
        cb_tick.symbol = snapshot.symbol;
        cb_tick.price = 100.0;
        cb_tick.received_ts_ms = now;
        CryptoLatencyTick kraken_tick = cb_tick;
        kraken_tick.source = QStringLiteral("kraken");
        kraken_tick.price = 110.0;
        kraken_tick.received_ts_ms = now;
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

    void stale_sources_do_not_count_as_live_confirmation() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        CryptoLatencySnapshot snapshot;
        snapshot.symbol = QStringLiteral("BTC-USD");
        CryptoLatencySourceState fresh;
        fresh.source = QStringLiteral("coinbase");
        fresh.last_tick_ms = now;
        CryptoLatencySourceState stale;
        stale.source = QStringLiteral("kraken");
        stale.last_tick_ms = now - 5001;
        snapshot.sources = {fresh, stale};

        const auto filtered = CryptoLatencyService::filtered_snapshot(snapshot,
                                                                        {QStringLiteral("coinbase"), QStringLiteral("kraken")});
        QCOMPARE(filtered.live_sources, 1);
    }

    void microstructure_uses_top_book_quantities_not_last_price_vs_midpoint() {
        using openmarketterminal::services::edge_radar::CryptoMicrostructureRadar;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        CryptoLatencySnapshot snapshot;
        snapshot.symbol = QStringLiteral("BTC-USD");
        snapshot.freshest_source = QStringLiteral("coinbase");
        snapshot.freshest_age_ms = 0;
        snapshot.mid_price = 100.0;
        snapshot.live_sources = 2;

        CryptoLatencySourceState coinbase;
        coinbase.source = QStringLiteral("coinbase");
        coinbase.status = QStringLiteral("live");
        coinbase.last_tick_ms = now;
        CryptoLatencySourceState kraken = coinbase;
        kraken.source = QStringLiteral("kraken");
        snapshot.sources = {coinbase, kraken};

        CryptoLatencyTick cb;
        cb.source = coinbase.source;
        cb.symbol = snapshot.symbol;
        cb.price = 100.0; // Exactly midpoint: the old proxy would have read zero.
        cb.best_bid = 99.0;
        cb.best_ask = 101.0;
        cb.bid_size = 9.0;
        cb.ask_size = 1.0;
        cb.received_ts_ms = now - 1000;
        cb.sequence = 1;
        CryptoLatencyTick kr = cb;
        kr.source = kraken.source;
        kr.bid_size = 8.0;
        kr.ask_size = 2.0;
        kr.received_ts_ms = now;
        kr.sequence = 2;
        snapshot.latest_ticks = {cb, kr};

        CryptoMicrostructureRadar radar;
        radar.add_tick(cb);
        radar.add_tick(kr);
        const auto flow = radar.snapshot(snapshot);

        QCOMPARE(flow.top_book_sources, 2);
        QVERIFY2(flow.book_pressure > 0.50,
                 "bid-heavy top book must produce positive imbalance even at midpoint price");
        QVERIFY(flow.microprice > 100.0);
        QCOMPARE(flow.aggressive_trade_flow_status,
                 QStringLiteral("warming: insufficient classified trade volume"));

        const QJsonObject json = CryptoMicrostructureRadar::to_json(flow);
        QVERIFY(json.value("microprice").toDouble() > 100.0);
        QCOMPARE(json.value("top_book_sources").toInt(), 2);
        const QJsonArray rows = json.value("sources").toArray();
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows.first().toObject().contains("top_book_imbalance"));
        QVERIFY(rows.first().toObject().contains("bid_size"));
    }

    void microstructure_uses_classified_executed_volume_for_aggressor_pressure() {
        using openmarketterminal::services::edge_radar::CryptoMicrostructureRadar;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        CryptoLatencySnapshot latency;
        latency.symbol = QStringLiteral("BTC-USD");
        latency.freshest_source = QStringLiteral("coinbase");
        latency.freshest_age_ms = 0;
        latency.mid_price = 100.0;
        latency.live_sources = 2;
        CryptoLatencySourceState cb_state;
        cb_state.source = QStringLiteral("coinbase");
        cb_state.status = QStringLiteral("live");
        cb_state.last_tick_ms = now;
        CryptoLatencySourceState gm_state = cb_state;
        gm_state.source = QStringLiteral("gemini");
        latency.sources = {cb_state, gm_state};

        CryptoMicrostructureRadar radar;
        CryptoLatencyTick anchor;
        anchor.source = QStringLiteral("coinbase");
        anchor.symbol = latency.symbol;
        anchor.price = 99.9;
        anchor.received_ts_ms = now - 6000;
        radar.add_tick(anchor);
        for (int i = 0; i < 6; ++i) {
            CryptoLatencyTick trade = anchor;
            trade.source = i % 2 ? QStringLiteral("gemini") : QStringLiteral("coinbase");
            trade.price = 100.0 + i * 0.01;
            trade.received_ts_ms = now - 4500 + i * 800;
            trade.is_trade = true;
            trade.aggressor_side = i < 5 ? QStringLiteral("buy") : QStringLiteral("sell");
            trade.trade_size = i < 5 ? 2.0 : 1.0;
            trade.sequence = i + 1;
            radar.add_tick(trade);
            if (i >= 4) latency.latest_ticks.push_back(trade);
        }

        const auto flow = radar.snapshot(latency);
        QCOMPARE(flow.classified_trades, 6);
        QCOMPARE(flow.aggressor_buy_volume, 10.0);
        QCOMPARE(flow.aggressor_sell_volume, 1.0);
        QCOMPARE(flow.aggressor_coverage, 1.0);
        QVERIFY(flow.aggressor_pressure > 0.81 && flow.aggressor_pressure < 0.82);
        QVERIFY(flow.aggressive_trade_flow_status.startsWith(QStringLiteral("available:")));
        QCOMPARE(CryptoMicrostructureRadar::to_json(flow).value("classified_trades").toInt(), 6);
    }

    void microstructure_requires_real_history_for_named_windows() {
        using openmarketterminal::services::edge_radar::CryptoMicrostructureRadar;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        CryptoLatencySnapshot snapshot;
        snapshot.symbol = QStringLiteral("BTC-USD");
        snapshot.freshest_source = QStringLiteral("coinbase");
        snapshot.freshest_age_ms = 0;
        snapshot.mid_price = 100.0;
        snapshot.live_sources = 1;
        CryptoLatencySourceState source;
        source.source = QStringLiteral("coinbase");
        source.status = QStringLiteral("live");
        source.last_tick_ms = now;
        snapshot.sources = {source};

        CryptoLatencyTick old_tick;
        old_tick.source = source.source;
        old_tick.symbol = snapshot.symbol;
        old_tick.price = 100.0;
        old_tick.received_ts_ms = now - 6000;
        old_tick.sequence = 1;
        CryptoLatencyTick recent_tick = old_tick;
        recent_tick.price = 101.0;
        recent_tick.received_ts_ms = now;
        recent_tick.sequence = 2;
        snapshot.latest_ticks = {recent_tick};

        CryptoMicrostructureRadar radar;
        radar.add_tick(old_tick);
        radar.add_tick(recent_tick);
        const auto flow = radar.snapshot(snapshot);
        const auto five = flow.windows[0];
        const auto fifteen = flow.windows[1];
        const auto sixty = flow.windows[2];
        QVERIFY(five.available);
        QVERIFY(!fifteen.available);
        QVERIFY(!sixty.available);
        QVERIFY(five.coverage_ms >= 5000);
        QVERIFY(fifteen.coverage_ms < 15000);
    }
};

QTEST_GUILESS_MAIN(TstFeedReconnect)
#include "tst_feed_reconnect.moc"
