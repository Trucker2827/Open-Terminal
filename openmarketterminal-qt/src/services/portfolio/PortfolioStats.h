// src/services/portfolio/PortfolioStats.h
#pragma once
// Pure portfolio risk-statistics helpers, split out of
// PortfolioService::compute_metrics so the Sharpe / Sortino / max-drawdown math
// can be unit-tested without standing up the service (DB + snapshots + network).
//
// Inputs match the service's snapshot path: return series are per-period
// percentages (e.g. a +1.5% day is 1.5), risk-free is also per-period percent.
// Formulas are reproduced verbatim from the inline code they replace.

#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace openmarketterminal::portfolio::stats {

/// Annualised Sharpe ratio from a per-period return series (in %), with the
/// risk-free rate per period (in %). Uses the SAMPLE standard deviation (n-1).
/// Returns nullopt (the metric is undefined) when there are fewer than 2 points
/// or volatility is ~0 — callers leave the optional metric unset in that case.
inline std::optional<double> sharpe_annualized(const QVector<double>& returns_pct,
                                               double rf_per_period_pct, double periods_per_year) {
    const qsizetype n = returns_pct.size();
    if (n < 2)
        return std::nullopt;
    double sum = 0.0;
    for (const double r : returns_pct)
        sum += r;
    const double mean = sum / static_cast<double>(n);
    double sum_sq = 0.0;
    for (const double r : returns_pct)
        sum_sq += (r - mean) * (r - mean);
    const double sd = std::sqrt(sum_sq / static_cast<double>(n - 1)); // sample std-dev
    if (sd <= 1e-6)
        return std::nullopt;
    return ((mean - rf_per_period_pct) / sd) * std::sqrt(periods_per_year);
}

/// Annualised Sortino ratio — like Sharpe but penalising only downside
/// deviation below the per-period risk-free rate (the minimum acceptable
/// return). Downside deviation uses the POPULATION divisor (n). Returns nullopt
/// when the series is empty or there is no (or ~0) downside deviation.
inline std::optional<double> sortino_annualized(const QVector<double>& returns_pct,
                                                double rf_per_period_pct, double periods_per_year) {
    const qsizetype n = returns_pct.size();
    if (n < 1)
        return std::nullopt;
    double sum = 0.0;
    for (const double r : returns_pct)
        sum += r;
    const double mean = sum / static_cast<double>(n);
    double downside_sq = 0.0;
    for (const double r : returns_pct) {
        const double shortfall = r - rf_per_period_pct;
        if (shortfall < 0.0)
            downside_sq += shortfall * shortfall;
    }
    const double downside_dev = std::sqrt(downside_sq / static_cast<double>(n)); // population
    if (downside_dev <= 1e-6)
        return std::nullopt;
    return ((mean - rf_per_period_pct) / downside_dev) * std::sqrt(periods_per_year);
}

/// Degraded cross-sectional risk estimate, used when a portfolio has too little
/// snapshot history for a real time-series volatility (fewer than 3 snapshots).
/// It treats the set of holdings' single-day change percentages as the sample.
struct CrossSectionalRisk {
    int n = 0;                     ///< count of non-negligible day changes used
    double mean_pct = 0;           ///< mean single-day change (%)
    double daily_vol_pct = 0;      ///< population std-dev of the day changes (%)
    double annualized_vol_pct = 0; ///< daily_vol_pct * sqrt(periods) — assign
                                   ///  DIRECTLY to a %-scaled volatility metric
};

/// Compute the degraded cross-sectional risk from a set of holdings' single-day
/// change percentages. Inputs are in PERCENT (a +1.5% day is 1.5); every output
/// is likewise in percent, so `annualized_vol_pct` is assigned straight to the
/// %-scaled volatility metric and must NOT be multiplied by 100 again (doing so
/// was a real bug — it disagreed with the snapshot path, inflating vol ~100×).
/// Day changes with magnitude <= 1e-4 are treated as unpriced/flat and skipped.
/// Uses the POPULATION variance divisor (n), matching the fallback path it
/// replaces. When fewer than 2 non-negligible day changes remain, all fields are
/// left 0 so the caller leaves the degraded metrics unset.
inline CrossSectionalRisk cross_sectional_risk(const QVector<double>& day_changes_pct,
                                               double periods_per_year) {
    double sum = 0.0;
    double sum_sq = 0.0;
    int n = 0;
    for (const double dcp : day_changes_pct) {
        if (std::abs(dcp) > 1e-4) {
            sum += dcp;
            sum_sq += dcp * dcp;
            ++n;
        }
    }
    CrossSectionalRisk r;
    if (n < 2)
        return r;
    r.n = n;
    r.mean_pct = sum / static_cast<double>(n);
    const double var = (sum_sq / static_cast<double>(n)) - (r.mean_pct * r.mean_pct);
    r.daily_vol_pct = std::sqrt(std::max(var, 0.0));
    r.annualized_vol_pct = r.daily_vol_pct * std::sqrt(periods_per_year);
    return r;
}

/// Maximum drawdown (%, <= 0) of an equity-value series: the most negative
/// (value - running_peak) / running_peak * 100 seen along the series. Returns 0
/// for an empty series or one that only ever rises.
inline double max_drawdown_pct(const QVector<double>& values) {
    if (values.isEmpty())
        return 0.0;
    double peak = values.first();
    double max_dd = 0.0;
    for (const double v : values) {
        peak = std::max(peak, v);
        if (peak > 1e-6)
            max_dd = std::min(max_dd, (v - peak) / peak * 100.0);
    }
    return max_dd;
}

} // namespace openmarketterminal::portfolio::stats
