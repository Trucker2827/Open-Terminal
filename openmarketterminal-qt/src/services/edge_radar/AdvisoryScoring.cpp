#include "services/edge_radar/AdvisoryScoring.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace openmarketterminal::adv {

namespace {

constexpr double kLogLossClampLow = 1e-6;
constexpr double kLogLossClampHigh = 1.0 - 1e-6;

double square(double x) { return x * x; }

double clamp_prob(double p) {
    return std::min(kLogLossClampHigh, std::max(kLogLossClampLow, p));
}

double brier_of(const QVector<ScoredRow>& rows, double ScoredRow::*field) {
    if (rows.isEmpty()) return 0.0;
    double sum = 0.0;
    for (const auto& row : rows) sum += square(row.*field - static_cast<double>(row.outcome));
    return sum / static_cast<double>(rows.size());
}

double logloss_of_pre(const QVector<ScoredRow>& rows) {
    if (rows.isEmpty()) return 0.0;
    double sum = 0.0;
    for (const auto& row : rows) {
        const double p = clamp_prob(row.p_pre);
        const double o = static_cast<double>(row.outcome);
        sum += -(o * std::log(p) + (1.0 - o) * std::log(1.0 - p));
    }
    return sum / static_cast<double>(rows.size());
}

// improvement_vs_daemon_pre for a given (possibly resampled) row set:
// brier_daemon - brier_pre, positive meaning pre beats daemon.
double improvement_vs_daemon(const QVector<ScoredRow>& rows) {
    const double brier_pre = brier_of(rows, &ScoredRow::p_pre);
    const double brier_daemon = brier_of(rows, &ScoredRow::daemon);
    return brier_daemon - brier_pre;
}

} // namespace

PairedResult score_paired(const QVector<ScoredRow>& rows, int bootstrap_iters, quint32 seed) {
    PairedResult result;
    result.n = rows.size();
    if (rows.isEmpty()) {
        // n=0: everything stays at the zero-initialized defaults above, no
        // NaNs (division by zero is avoided entirely by not dividing).
        return result;
    }

    result.brier_pre = brier_of(rows, &ScoredRow::p_pre);
    result.brier_post = brier_of(rows, &ScoredRow::p_post);
    result.brier_market = brier_of(rows, &ScoredRow::market);
    result.brier_daemon = brier_of(rows, &ScoredRow::daemon);
    result.logloss_pre = logloss_of_pre(rows);
    result.improvement_vs_market_pre = result.brier_market - result.brier_pre;
    result.improvement_vs_daemon_pre = result.brier_daemon - result.brier_pre;

    // Percentile bootstrap CI on the PAIRED improvement of pre vs daemon
    // (improvement_vs_daemon_pre): resample rows WITH replacement
    // `bootstrap_iters` times using a fixed-seed std::mt19937 (deterministic,
    // never wall-clock/random_device), recompute improvement_vs_daemon on
    // each resample, then take the 2.5th/97.5th percentiles of that
    // distribution as ci_low/ci_high.
    if (bootstrap_iters > 0) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> pick(0, rows.size() - 1);
        QVector<double> samples;
        samples.reserve(bootstrap_iters);
        QVector<ScoredRow> resample;
        resample.reserve(rows.size());
        for (int iter = 0; iter < bootstrap_iters; ++iter) {
            resample.clear();
            for (int i = 0; i < rows.size(); ++i) resample.push_back(rows[pick(rng)]);
            samples.push_back(improvement_vs_daemon(resample));
        }
        std::sort(samples.begin(), samples.end());
        auto percentile = [&samples](double p) -> double {
            if (samples.isEmpty()) return 0.0;
            const double idx = p * static_cast<double>(samples.size() - 1);
            const int lo = static_cast<int>(std::floor(idx));
            const int hi = static_cast<int>(std::ceil(idx));
            if (lo == hi) return samples[lo];
            const double frac = idx - static_cast<double>(lo);
            return samples[lo] + frac * (samples[hi] - samples[lo]);
        };
        result.ci_low = percentile(0.025);
        result.ci_high = percentile(0.975);
    } else {
        // No bootstrap requested: collapse the CI onto the point estimate.
        result.ci_low = result.improvement_vs_daemon_pre;
        result.ci_high = result.improvement_vs_daemon_pre;
    }

    return result;
}

Participation participation(const QVector<QString>& states) {
    Participation p;
    p.opened = states.size();
    for (const QString& state : states) {
        if (state == QStringLiteral("COMMITTED_BLIND")) ++p.committed_blind;
        else if (state == QStringLiteral("REVEALED")) ++p.revealed;
        else if (state == QStringLiteral("COMMITTED_POST")) ++p.committed_post;
        else if (state == QStringLiteral("EXPIRED")) ++p.expired;
        else if (state == QStringLiteral("ABANDONED")) ++p.abandoned;
    }
    if (p.opened > 0) {
        p.open_to_commit_rate = static_cast<double>(p.committed_blind + p.revealed + p.committed_post) /
                                 static_cast<double>(p.opened);
        p.expiration_rate = static_cast<double>(p.expired) / static_cast<double>(p.opened);
    } else {
        p.open_to_commit_rate = 0.0;
        p.expiration_rate = 0.0;
    }
    return p;
}

} // namespace openmarketterminal::adv
