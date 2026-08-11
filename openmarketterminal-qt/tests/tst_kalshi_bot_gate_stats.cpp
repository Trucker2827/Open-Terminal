#include <QtTest>

#include "services/prediction/kalshi/KalshiBotGateStats.h"

#include <cmath>
#include <random>
#include <vector>

using namespace openmarketterminal::services::prediction::kalshi_ns;

namespace {

/// A record with a KNOWN edge and known volatility, so a claim about the
/// interval can be checked against the truth rather than against itself.
std::vector<double> synthetic(double mean, double sd, int n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> draw(mean, sd);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(draw(rng));
    return out;
}

} // namespace

class TstKalshiBotGateStats : public QObject {
    Q_OBJECT

  private slots:
    // The property the whole change rests on: a record with NO edge must not
    // produce an interval that excludes zero. `net_pnl > 0` passes such a
    // record half the time; this must not.
    void a_no_edge_record_does_not_exclude_zero() {
        const auto values = synthetic(0.0, 1.35, 300, 11);
        const auto ci = bootstrap_mean_interval(values, 0.95, 20260811, 4000);
        QVERIFY(ci.available);
        QVERIFY2(ci.lo <= 0.0, "a zero-edge record must not read as a distinguishable edge");
        QVERIFY(ci.hi >= 0.0);
        QVERIFY(ci.lo < ci.hi);
    }

    // ...and a record with a large, real edge must clear it, or the criterion
    // would reject everything and be just as useless in the other direction.
    void a_large_real_edge_excludes_zero() {
        const auto values = synthetic(0.40, 1.35, 300, 12);
        const auto ci = bootstrap_mean_interval(values, 0.95, 20260811, 4000);
        QVERIFY(ci.available);
        QVERIFY2(ci.lo > 0.0, "a +$0.40/bid edge over 300 bids must be distinguishable from zero");
    }

    // The interval must bracket the sample mean, and tighten as confidence
    // drops — the two sanity properties that catch a transposed quantile.
    void the_interval_brackets_the_mean_and_widens_with_confidence() {
        const auto values = synthetic(0.05, 1.0, 200, 13);
        const auto wide = bootstrap_mean_interval(values, 0.99, 20260811, 4000);
        const auto narrow = bootstrap_mean_interval(values, 0.90, 20260811, 4000);
        QVERIFY(wide.lo <= wide.mean && wide.mean <= wide.hi);
        QVERIFY(narrow.lo <= narrow.mean && narrow.mean <= narrow.hi);
        QVERIFY2(wide.hi - wide.lo > narrow.hi - narrow.lo,
                 "a 99% interval must be wider than a 90% one");
    }

    // A sealed gate must return the same verdict on a re-run of the same
    // record. Same seed and input => identical numbers, bit for bit.
    void the_same_seed_gives_identical_results() {
        const auto values = synthetic(0.1, 1.0, 150, 14);
        const auto a = bootstrap_mean_interval(values, 0.95, 20260811, 2000);
        const auto b = bootstrap_mean_interval(values, 0.95, 20260811, 2000);
        QCOMPARE(a.lo, b.lo);
        QCOMPARE(a.hi, b.hi);
        const auto d1 = bootstrap_drawdown_noise(values, 300, 20260811, 2000);
        const auto d2 = bootstrap_drawdown_noise(values, 300, 20260811, 2000);
        QCOMPARE(d1.p50, d2.p50);
        QCOMPARE(d1.p95, d2.p95);
        // A different seed must actually resample differently, or "deterministic"
        // would be hiding a constant.
        const auto c = bootstrap_mean_interval(values, 0.95, 999, 2000);
        QVERIFY(!qFuzzyCompare(a.lo, c.lo) || !qFuzzyCompare(a.hi, c.hi));
    }

    // Degenerate inputs are UNAVAILABLE, never a silently-passing zero — the
    // same rule the Brier criterion already follows.
    void degenerate_input_is_unavailable_never_zero() {
        QVERIFY(!bootstrap_mean_interval({}, 0.95, 1, 100).available);
        QVERIFY(!bootstrap_mean_interval({0.5}, 0.95, 1, 100).available);
        QVERIFY(!bootstrap_mean_interval({0.5, 0.5}, 0.0, 1, 100).available);
        QVERIFY(!bootstrap_mean_interval({0.5, 0.5}, 1.0, 1, 100).available);
        QVERIFY(!bootstrap_drawdown_noise({}, 300, 1, 100).available);
        QVERIFY(!bootstrap_drawdown_noise({0.1, -0.1}, 0, 1, 100).available);
    }

    // The noise floor must describe NOISE, not the record's edge: a strongly
    // profitable record and the same record with the edge removed must imply
    // the same drawdown, because the function re-centres before walking.
    void the_noise_floor_ignores_the_records_edge() {
        const auto flat = synthetic(0.0, 1.35, 200, 15);
        std::vector<double> lifted;
        lifted.reserve(flat.size());
        for (double v : flat) lifted.push_back(v + 5.0);  // hugely profitable
        const auto a = bootstrap_drawdown_noise(flat, 300, 20260811, 3000);
        const auto b = bootstrap_drawdown_noise(lifted, 300, 20260811, 3000);
        QVERIFY(a.available && b.available);
        QCOMPARE(a.p50, b.p50);
        QCOMPARE(a.p95, b.p95);
    }

    // Drawdown grows with the length of the record and with volatility. This is
    // the property that makes a FLAT dollar cap wrong: the same cap means
    // something different at 100 bids than at 300.
    void the_noise_floor_grows_with_length_and_volatility() {
        const auto values = synthetic(0.0, 1.35, 200, 16);
        const auto short_run = bootstrap_drawdown_noise(values, 100, 20260811, 3000);
        const auto long_run = bootstrap_drawdown_noise(values, 300, 20260811, 3000);
        QVERIFY2(long_run.p50 > short_run.p50, "300 bids must imply a deeper drawdown than 100");

        const auto calm = bootstrap_drawdown_noise(synthetic(0.0, 0.5, 200, 17), 300,
                                                   20260811, 3000);
        QVERIFY2(long_run.p50 > calm.p50, "a noisier record must imply a deeper drawdown");
        QVERIFY(long_run.p95 > long_run.p50);
    }

    // The measured reality this change exists for: at the live record's own
    // volatility, a $20 cap sits BELOW the median drawdown of a no-edge record,
    // so it cannot separate skill from noise. Pinned so nobody re-seals $20
    // believing it is conservative.
    void twenty_dollars_sits_below_the_noise_floor_at_live_volatility() {
        const auto values = synthetic(0.0, 1.35, 300, 18);
        const auto noise = bootstrap_drawdown_noise(values, 300, 20260811, 4000);
        QVERIFY(noise.available);
        QVERIFY2(noise.p50 > 20.0,
                 "a $20 cap is below the median no-edge drawdown at this volatility");
        QVERIFY2(noise.p95 < 100.0,
                 "and $100 is far above even the 95th percentile, so it would never bind");
    }
};

QTEST_MAIN(TstKalshiBotGateStats)
#include "tst_kalshi_bot_gate_stats.moc"
