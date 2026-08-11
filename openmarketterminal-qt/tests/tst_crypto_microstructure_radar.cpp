#include <QtTest>
#include "services/crypto/CryptoFees.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"
#include "services/edge_radar/VolCostContext.h"

using namespace openmarketterminal::services::edge_radar;
namespace latency = openmarketterminal::services::crypto_latency;
namespace fees = openmarketterminal::services::crypto;

// Roadmap Pillar 1: a displayed opportunity is net-of-round-trip for the
// user's actual tier, never gross. With a cost context, the radar must not
// emit TRADE CANDIDATE unless the observed move clears the fee hurdle and
// (when realized vol exists) stands above the ambient noise floor. Missing
// vol fails OPEN — matching the CLI scalp-gate blocker — but is stated.
class TstCryptoMicrostructureRadar : public QObject {
    Q_OBJECT

    // Steady one-direction trend: `count` ticks 1s apart ending at end_ms.
    static void feed_trend(CryptoMicrostructureRadar& radar, qint64 end_ms, double base,
                           double step, int count) {
        for (int i = 0; i < count; ++i) {
            latency::CryptoLatencyTick t;
            t.source = QStringLiteral("coinbase");
            t.symbol = QStringLiteral("BTC-USD");
            t.price = base + step * i;
            t.received_ts_ms = end_ms - static_cast<qint64>(count - 1 - i) * 1000;
            t.sequence = i;
            radar.add_tick(t);
        }
    }

    static latency::CryptoLatencySnapshot live_snapshot() {
        latency::CryptoLatencySnapshot ls;
        ls.symbol = QStringLiteral("BTC-USD");
        ls.freshest_source = QStringLiteral("coinbase");
        ls.freshest_age_ms = 400;
        ls.mid_price = 100000.0;
        ls.cross_source_spread_bps = 1.0;
        ls.live_sources = 2;
        latency::CryptoLatencySourceState a;
        a.source = QStringLiteral("coinbase");
        a.status = QStringLiteral("live");
        a.ticks = 100;
        latency::CryptoLatencySourceState b;
        b.source = QStringLiteral("kraken");
        b.status = QStringLiteral("live");
        b.ticks = 90;
        ls.sources = {a, b};
        return ls;
    }

    // Coinbase tier-1 taker round trip (60x2) + 5bps slippage; the observed
    // 1bps cross-source divergence brings the hurdle to 126bps.
    static CryptoMicrostructureCostContext fee_context() {
        CryptoMicrostructureCostContext ctx;
        ctx.fee_available = true;
        ctx.venue_key = QStringLiteral("coinbase_advanced");
        ctx.round_trip_fee_bps = 120.0;
        ctx.slippage_bps = 5.0;
        return ctx;
    }

private slots:
    // Control: without a cost context the legacy pressure-only call is
    // unchanged — this is the CLI capture path, whose gating lives in the
    // scalp gate itself.
    void pressure_only_candidate_without_context() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 0.5, 61);
        const auto snap = radar.snapshot(live_snapshot());
        QCOMPARE(snap.call, QString("TRADE CANDIDATE"));
        QVERIFY(!snap.cost_context_available);
        QVERIFY(snap.net_economics_status.contains(QStringLiteral("unavailable")));
        QVERIFY(snap.noise_floor_status.contains(QStringLiteral("unavailable")));
    }

    // (a) The same pressure setup whose move cannot clear the round-trip
    // hurdle must downgrade, naming net bps vs the hurdle.
    void net_below_hurdle_blocks_candidate() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 0.5, 61);
        const auto snap = radar.snapshot(live_snapshot(), fee_context());
        QCOMPARE(snap.call, QString("WATCH"));
        QVERIFY(snap.cost_context_available);
        QVERIFY(snap.observed_move_available);
        QVERIFY(snap.observed_move_bps > 0.0 && snap.observed_move_bps < 2.0);
        QVERIFY(qAbs(snap.round_trip_hurdle_bps - 126.0) < 1e-9);
        QVERIFY(snap.net_move_bps < 0.0);
        QVERIFY(snap.rationale.contains(QStringLiteral("hurdle")));
        QVERIFY(snap.rationale.contains(QStringLiteral("net")));
    }

    void candidate_when_net_clears_hurdle() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 100.0, 61);
        auto ctx = fee_context();
        ctx.vol_available = true;
        ctx.realized_vol_per_min_bps = 40.0;
        ctx.realized_vol_samples = 120;
        const auto snap = radar.snapshot(live_snapshot(), ctx);
        QCOMPARE(snap.call, QString("TRADE CANDIDATE"));
        QVERIFY(snap.net_move_bps > 0.0);
        QCOMPARE(snap.net_economics_status, QString("available"));
        QVERIFY(snap.noise_sigma_available);
        QVERIFY(snap.observed_move_sigma >= 1.0);
    }

    // (b) Sigma is computed from the supplied vol input with sqrt-time
    // scaling over the primary observation window.
    void sigma_computed_from_vol_input() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 100.0, 61);
        auto ctx = fee_context();
        ctx.vol_available = true;
        ctx.realized_vol_per_min_bps = 40.0;
        const auto snap = radar.snapshot(live_snapshot(), ctx);
        QVERIFY(snap.noise_sigma_available);
        QCOMPARE(snap.observed_move_window_sec, 15);
        const double expected = std::abs(snap.observed_move_bps) /
                                (40.0 * std::sqrt(15.0 / 60.0));
        QVERIFY(qAbs(snap.observed_move_sigma - expected) < 1e-9);
    }

    // A move that clears the fee hurdle but sits inside ambient noise is
    // still not a candidate when vol IS available.
    void noise_floor_blocks_move_within_sigma() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 100.0, 61);
        auto ctx = fee_context();
        ctx.vol_available = true;
        ctx.realized_vol_per_min_bps = 4000.0;
        const auto snap = radar.snapshot(live_snapshot(), ctx);
        QCOMPARE(snap.call, QString("WATCH"));
        QVERIFY(snap.net_move_bps > 0.0);
        QVERIFY(snap.observed_move_sigma < 1.0);
        QVERIFY(snap.rationale.contains(QStringLiteral("noise")));
    }

    // (c) Unavailable vol fails OPEN (same semantics as the scalp-gate noise
    // blocker) but the snapshot states that vol is unavailable.
    void vol_unavailable_fails_open_and_is_stated() {
        CryptoMicrostructureRadar radar;
        feed_trend(radar, QDateTime::currentMSecsSinceEpoch(), 100000.0, 100.0, 61);
        const auto snap = radar.snapshot(live_snapshot(), fee_context());
        QCOMPARE(snap.call, QString("TRADE CANDIDATE"));
        QVERIFY(!snap.noise_sigma_available);
        QCOMPARE(snap.observed_move_sigma, 0.0);
        QVERIFY(snap.noise_floor_status.contains(QStringLiteral("unavailable")));
    }

    // The shared venue fee table is the single source for tier constants:
    // Coinbase tier 1 is 40bps maker / 60bps taker for GUI and CLI alike.
    void shared_fee_table_is_single_source() {
        QCOMPARE(fees::crypto_fee_venue_key(QStringLiteral("coinbase")),
                 QString("coinbase_advanced"));
        const auto tier = fees::coinbase_fee_tier_by_key(QStringLiteral("coinbase_advanced"));
        QVERIFY(tier.has_value());
        QCOMPARE(tier->maker_bps, 40.0);
        QCOMPARE(tier->taker_bps, 60.0);
        const auto profile = fees::crypto_default_fee_profile(QStringLiteral("coinbase_advanced"));
        QCOMPARE(profile.maker_bps, 40.0);
        QCOMPARE(profile.taker_bps, 60.0);
    }

    // The radar's veto has been correct and unreachable: nothing on the live
    // path supplied a fee context, so every CLI snapshot reported
    // `cost_context_available = false` and the call stayed GROSS. with_fee_context
    // is the missing half. It must agree with the scalp gate's arithmetic
    // ((fee_bps * 2.0) + slippage_bps, EdgeJournalCommandsC.cpp) or the radar and
    // the gate would disagree about the same trade.
    void fee_context_matches_the_scalp_gate_arithmetic() {
        CryptoMicrostructureCostContext vol;
        vol.vol_available = true;
        vol.realized_vol_per_min_bps = 2.847;
        vol.realized_vol_samples = 356;

        const auto merged = with_fee_context(
            vol, CryptoFeeInputs{QStringLiteral("coinbase_advanced"), 60.0, 5.0});
        QVERIFY(merged.fee_available);
        QCOMPARE(merged.venue_key, QString("coinbase_advanced"));
        QCOMPARE(merged.round_trip_fee_bps, 120.0); // 60 one-way, entry + exit
        QCOMPARE(merged.slippage_bps, 5.0);
        // The vol half must survive untouched — it is a separate estimate.
        QVERIFY(merged.vol_available);
        QCOMPARE(merged.realized_vol_per_min_bps, 2.847);
        QCOMPARE(merged.realized_vol_samples, 356);
    }

    // An absent profile must read as UNKNOWN, never as zero cost — that is the
    // whole reason the radar has a separate availability flag. A zero RATE is
    // different: waivers and rebate tiers are real, and must gate normally.
    void absent_profile_stays_unknown_but_zero_rate_is_real() {
        const CryptoMicrostructureCostContext empty;

        const auto absent = with_fee_context(empty, CryptoFeeInputs{});
        QVERIFY(!absent.fee_available);
        QCOMPARE(absent.round_trip_fee_bps, 0.0);

        const auto waived = with_fee_context(
            empty, CryptoFeeInputs{QStringLiteral("coinbase_advanced"), 0.0, 0.0});
        QVERIFY(waived.fee_available);
        QCOMPARE(waived.round_trip_fee_bps, 0.0);
    }

    // End to end through the radar: the same trend that reads TRADE CANDIDATE
    // gross must downgrade once the fee half is supplied by with_fee_context —
    // not just by the test's hand-built context.
    void wired_fee_context_downgrades_a_gross_candidate() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        CryptoMicrostructureRadar gross;
        feed_trend(gross, now, 100000.0, 0.5, 61);
        QCOMPARE(gross.snapshot(live_snapshot()).call, QString("TRADE CANDIDATE"));

        CryptoMicrostructureRadar netted;
        feed_trend(netted, now, 100000.0, 0.5, 61);
        const auto snap = netted.snapshot(
            live_snapshot(),
            with_fee_context(CryptoMicrostructureCostContext{},
                             CryptoFeeInputs{QStringLiteral("coinbase_advanced"), 60.0, 5.0}));
        QCOMPARE(snap.call, QString("WATCH"));
        QVERIFY(snap.cost_context_available);
        QVERIFY(snap.rationale.contains(QStringLiteral("hurdle")));
    }
};
QTEST_MAIN(TstCryptoMicrostructureRadar)
#include "tst_crypto_microstructure_radar.moc"
