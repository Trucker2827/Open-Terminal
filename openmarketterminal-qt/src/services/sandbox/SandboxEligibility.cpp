#include "services/sandbox/SandboxEligibility.h"

#include <algorithm>

namespace openmarketterminal::services::sandbox {

EligibilityVerdict evaluate_eligibility(const EligibilityInput& in) {
    EligibilityVerdict v;

    // Hypothetical books are always blocked, unconditionally, before any
    // other bar is checked -- see file header.
    if (in.hypothetical) {
        v.blockers << QStringLiteral("hypothetical instrument");
        return v;
    }

    if (in.active_days < kMinActiveDays)
        v.blockers << QStringLiteral("insufficient active days (%1 < %2)").arg(in.active_days).arg(kMinActiveDays);

    if (in.resolved < kMinResolvedSample)
        v.blockers << QStringLiteral("insufficient resolved sample (%1 < %2)").arg(in.resolved).arg(kMinResolvedSample);

    // Bar is strictly `> 0` -- exactly 0 (or negative) is blocked, per
    // spec-§8: "hasn't lost money yet" is not a demonstrated edge.
    if (!(in.net_pnl > 0))
        v.blockers << QStringLiteral("no demonstrated edge (net_pnl %1 <= 0)").arg(in.net_pnl);

    // gross_notional <= 0 means no resolved notional at all (0/0 is not
    // evidence of a blown drawdown) -- treated as a pass on this bar; in
    // practice such a book already fails the resolved-sample bar above.
    if (in.gross_notional > 0) {
        const double drawdown_frac = in.max_drawdown / in.gross_notional;
        if (drawdown_frac > kMaxDrawdownFrac)
            v.blockers << QStringLiteral("drawdown exceeds cap (%1% > %2%)")
                              .arg(drawdown_frac * 100.0)
                              .arg(kMaxDrawdownFrac * 100.0);
    }

    // total_positions is ALL states (see file header); degraded is never
    // inflated by 'unknown' rows (LeaderboardRow::degraded already excludes
    // them), but the denominator still counts every position of the book.
    const int denom = std::max(1, in.total_positions);
    const double degraded_share = static_cast<double>(in.degraded) / denom;
    if (degraded_share >= kMaxDegradedShare)
        v.blockers << QStringLiteral("degraded share too high (%1% >= %2%)")
                          .arg(degraded_share * 100.0)
                          .arg(kMaxDegradedShare * 100.0);

    v.eligible = v.blockers.isEmpty();
    return v;
}

} // namespace openmarketterminal::services::sandbox
