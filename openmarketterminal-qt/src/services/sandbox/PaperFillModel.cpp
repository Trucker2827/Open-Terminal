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
    // Last tick at or before expires_at -- the only legal expiry close
    // print. Post-expiry ticks are dead to this position in BOTH roles:
    // they can neither trigger target/stop nor set the expiry close price
    // (PnL-integrity: an expired position must not harvest later moves).
    const TickRow* last_pre_expiry = nullptr;
    for (const TickRow& tick : ticks) {
        if (tick.ts_ms > expires_at) {
            continue; // post-expiry print: not ours to trade
        }
        last_pre_expiry = &tick;
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
        if (last_pre_expiry == nullptr) {
            // Data gap: no tick at or before expiry to report a real exit
            // price against. Caller is responsible for flagging this rather
            // than trusting a bare price-0 exit. ts is expires_at -- the
            // position legally died at expiry, not at observation time.
            return ExitResult{true, QStringLiteral("expiry"), 0.0, expires_at};
        }
        return ExitResult{true, QStringLiteral("expiry"), last_pre_expiry->price, expires_at};
    }
    return ExitResult{};
}

FillResult try_maker_fill(const QString& side, double limit_price, const QVector<TickRow>& ticks,
                          qint64 entry_deadline_ms, double through_bps) {
    const bool short_side = is_short_side(side);
    // A buy rests below the market and needs a tick at/through its limit; a
    // through margin pushes that threshold further away (harder to fill).
    const double margin = limit_price * through_bps / 10000.0;
    const double buy_threshold = limit_price - margin;
    const double sell_threshold = limit_price + margin;
    for (const TickRow& tick : ticks) {
        if (tick.ts_ms > entry_deadline_ms) {
            continue;
        }
        const bool qualifies = short_side ? (tick.price >= sell_threshold)
                                          : (tick.price <= buy_threshold);
        if (qualifies) {
            return FillResult{true, limit_price, tick.ts_ms};
        }
    }
    return FillResult{};
}

double effective_taker_price(const QString& side, double reference_price,
                             double half_spread_bps, double slippage_bps) {
    const double adverse = (half_spread_bps + slippage_bps) / 10000.0;
    return is_short_side(side) ? reference_price * (1.0 - adverse)
                               : reference_price * (1.0 + adverse);
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

double honest_round_trip_pnl(const QString& side, bool maker, double entry_price, double exit_price,
                             double qty, double entry_fee, double exit_fee,
                             double half_spread_bps, double slippage_bps) {
    double adj_entry = entry_price;
    double adj_exit = exit_price;
    if (!maker) {
        // Taker crosses half-spread + slippage on BOTH legs: adverse at the
        // position side on entry, adverse at the closing (opposite) side on
        // exit. A maker earns the spread and keeps the raw prices.
        const QString close_side = is_short_side(side) ? QStringLiteral("buy") : QStringLiteral("sell");
        adj_entry = effective_taker_price(side, entry_price, half_spread_bps, slippage_bps);
        adj_exit = effective_taker_price(close_side, exit_price, half_spread_bps, slippage_bps);
    }
    return realized_pnl(side, adj_entry, adj_exit, qty, entry_fee, exit_fee);
}

} // namespace openmarketterminal::services::sandbox
