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
