#include "services/prediction/kalshi/KalshiBotGateStats.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

/// Fixed generator, seeded by the caller. std::mt19937_64 is specified by the
/// standard, so the same seed gives the same draws on every platform and every
/// re-run -- which is what makes a sealed verdict reproducible.
std::mt19937_64 generator(std::uint64_t seed) { return std::mt19937_64(seed); }

double quantile_of_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const double pos = q * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return sorted[lo];
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

} // namespace

MeanInterval bootstrap_mean_interval(const std::vector<double>& values,
                                     double confidence,
                                     std::uint64_t seed,
                                     int samples) {
    MeanInterval out;
    if (values.size() < 2 || samples < 1 || !(confidence > 0.0) || !(confidence < 1.0))
        return out;  // unavailable, never a silently-passing zero

    out.mean = std::accumulate(values.begin(), values.end(), 0.0) /
               static_cast<double>(values.size());

    auto rng = generator(seed);
    std::uniform_int_distribution<std::size_t> pick(0, values.size() - 1);
    std::vector<double> means;
    means.reserve(static_cast<std::size_t>(samples));
    for (int s = 0; s < samples; ++s) {
        double total = 0.0;
        for (std::size_t i = 0; i < values.size(); ++i) total += values[pick(rng)];
        means.push_back(total / static_cast<double>(values.size()));
    }
    std::sort(means.begin(), means.end());

    const double tail = (1.0 - confidence) / 2.0;
    out.lo = quantile_of_sorted(means, tail);
    out.hi = quantile_of_sorted(means, 1.0 - tail);
    out.available = true;
    return out;
}

DrawdownNoise bootstrap_drawdown_noise(const std::vector<double>& values,
                                       int length,
                                       std::uint64_t seed,
                                       int samples) {
    DrawdownNoise out;
    if (values.size() < 2 || length < 1 || samples < 1) return out;

    // Remove the edge, keep the volatility: the question is what a NO-edge
    // record of this size and shape would do.
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) /
                        static_cast<double>(values.size());
    std::vector<double> centred;
    centred.reserve(values.size());
    for (double v : values) centred.push_back(v - mean);

    auto rng = generator(seed);
    std::uniform_int_distribution<std::size_t> pick(0, centred.size() - 1);
    std::vector<double> drawdowns;
    drawdowns.reserve(static_cast<std::size_t>(samples));
    for (int s = 0; s < samples; ++s) {
        double running = 0.0;
        double peak = 0.0;
        double worst = 0.0;
        for (int i = 0; i < length; ++i) {
            running += centred[pick(rng)];
            peak = std::max(peak, running);
            worst = std::max(worst, peak - running);
        }
        drawdowns.push_back(worst);
    }
    std::sort(drawdowns.begin(), drawdowns.end());

    out.p50 = quantile_of_sorted(drawdowns, 0.50);
    out.p95 = quantile_of_sorted(drawdowns, 0.95);
    out.available = true;
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
