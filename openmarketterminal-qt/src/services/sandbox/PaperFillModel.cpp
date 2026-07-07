#include "services/sandbox/PaperFillModel.h"

namespace openmarketterminal::services::sandbox {

namespace {

bool is_short_side(const QString& side) {
    return side == QLatin1String("short") || side == QLatin1String("sell");
}

} // namespace

FillResult try_fill(const QString& side, double limit_price, const QVector<TickRow>& ticks,
                     qint64 entry_deadline_ms) {
    const bool short_side = is_short_side(side);
    for (const TickRow& tick : ticks) {
        if (tick.ts_ms > entry_deadline_ms) {
            continue;
        }
        const bool qualifies = short_side ? (tick.price >= limit_price) : (tick.price <= limit_price);
        if (qualifies) {
            return FillResult{true, limit_price, tick.ts_ms};
        }
    }
    return FillResult{};
}

ExitResult check_exit(const QString& side, double target_price, double stop_price, qint64 expires_at,
                       const QVector<TickRow>& ticks, qint64 now_ms) {
    const bool short_side = is_short_side(side);
    for (const TickRow& tick : ticks) {
        // Stop is checked before target on every tick: this is a no-op for
        // the overwhelming majority of ticks (which satisfy at most one of
        // the two), and only decides the outcome in the degenerate case
        // where a single tick's price satisfies both bounds at once
        // (only possible with a misconfigured stop_price/target_price that
        // don't bracket disjoint ranges) -- there, ordering within the tick
        // is unknowable, so the conservative stop-wins rule applies.
        // Genuinely separate ticks are unaffected: whichever bound a tick
        // alone satisfies is returned immediately, preserving first-in-
        // tick-order semantics for the normal (non-degenerate) case.
        const bool stop_hit = short_side ? (tick.price >= stop_price) : (tick.price <= stop_price);
        const bool target_hit = short_side ? (tick.price <= target_price) : (tick.price >= target_price);
        if (stop_hit) {
            return ExitResult{true, QStringLiteral("stop"), tick.price, tick.ts_ms};
        }
        if (target_hit) {
            return ExitResult{true, QStringLiteral("target"), tick.price, tick.ts_ms};
        }
    }
    if (now_ms >= expires_at) {
        if (ticks.isEmpty()) {
            // Data gap: no ticks to report a real exit price against.
            // Caller is responsible for flagging this rather than trusting
            // a bare price-0 exit.
            return ExitResult{true, QStringLiteral("expiry"), 0.0, now_ms};
        }
        const TickRow& last = ticks.last();
        return ExitResult{true, QStringLiteral("expiry"), last.price, last.ts_ms};
    }
    return ExitResult{};
}

double fee_for(double notional, double bps) {
    return notional * bps / 10000.0;
}

double realized_pnl(const QString& side, double entry_price, double exit_price, double qty, double entry_fee,
                     double exit_fee) {
    const bool short_side = is_short_side(side);
    const double gross = short_side ? (entry_price - exit_price) * qty : (exit_price - entry_price) * qty;
    return gross - entry_fee - exit_fee;
}

} // namespace openmarketterminal::services::sandbox
