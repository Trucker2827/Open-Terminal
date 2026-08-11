#pragma once

// Resampling statistics for the sealed promotion gate. Pure: a vector of
// per-bid realized P&L in, numbers out. No Qt JSON, no I/O, no clock — so the
// gate's arithmetic can be tested exhaustively against synthetic records.
//
// WHY THIS EXISTS. Measured on the live record (114 settled bids, per-bid
// sd $1.35), bootstrapping 20,000 runs of 300 bids showed the gate's two
// money criteria were both wrong, in opposite directions:
//
//   * `net_pnl_usd > 0` is NOT a test. A family with EXACTLY zero edge passes
//     it 50.4% of the time -- it is a coin flip. Preregister six families and
//     the chance at least one no-edge family clears both money criteria is
//     84.8%. Promotion would be measuring luck.
//
//   * `max_drawdown_usd <= 20` rejects skill. A driftless record of that size
//     has a median peak-to-trough of $25.40, so a break-even family fails the
//     $20 cap 70.8% of the time and a genuinely profitable one (+2c/bid)
//     still fails 63.9%. The $50 ceiling already in KalshiBotGate is fine --
//     it is exceeded only 5.5% of the time -- but nothing told the operator
//     that $20 sat below the noise floor.
//
// So: a significance test for the skill question, and a reported noise floor
// so a drawdown cap can be preregistered knowingly instead of guessed.

#include <cstdint>
#include <vector>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// A percentile interval for the MEAN of `values`, from an ordinary
/// non-parametric bootstrap. `lo > 0` is the claim "this record's edge is
/// distinguishable from zero at `confidence`".
struct MeanInterval {
    double lo = 0.0;
    double hi = 0.0;
    double mean = 0.0;
    /// False when there is nothing to resample (fewer than two values, or a
    /// degenerate confidence). A criterion must then report UNAVAILABLE rather
    /// than treat an unmeasured interval as a pass.
    bool available = false;
};

/// The peak-to-trough drawdown a record with THIS record's own per-bid
/// distribution would produce with no edge at all -- the yardstick that says
/// whether an observed drawdown is a risk event or just noise.
struct DrawdownNoise {
    double p50 = 0.0;
    double p95 = 0.0;
    bool available = false;
};

/// Bootstrap the mean of `values`. `seed` is REQUIRED and the generator is
/// fixed: a sealed gate that returned a different verdict on a re-run of the
/// same record would be worse than no gate at all.
MeanInterval bootstrap_mean_interval(const std::vector<double>& values,
                                     double confidence,
                                     std::uint64_t seed,
                                     int samples = 4000);

/// Bootstrap the max drawdown of a `length`-step walk drawn from `values`
/// RE-CENTRED to zero mean, i.e. the same volatility with the edge removed.
/// Re-centring is the point: this must answer "what would noise alone do",
/// not "what did this record do".
DrawdownNoise bootstrap_drawdown_noise(const std::vector<double>& values,
                                       int length,
                                       std::uint64_t seed,
                                       int samples = 4000);

} // namespace openmarketterminal::services::prediction::kalshi_ns
