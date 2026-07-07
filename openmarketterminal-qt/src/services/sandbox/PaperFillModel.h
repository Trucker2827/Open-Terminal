#pragma once
// PaperFillModel.h — pure fill/exit rules for the Strategy Sandbox paper
// executor (Task 4). Every function here is a pure computation over its
// arguments: no I/O, no Database, no Qt-global state. Callers own fetching
// ticks (TickTail::ticks_since), fee schedules, and notionals.
//
// Sides: "buy" and "long" behave identically -- a resting limit BUY fills
// at/below limit_price, and for an open long position stop triggers at/below
// stop_price while target triggers at/above target_price. "sell" and "short"
// mirror in both directions (fill at/above limit; stop at/above, target
// at/below). Side strings are matched case-sensitively against the exact
// lowercase literals "buy"/"long"/"sell"/"short", same convention as the
// rest of services/ (see PaperVenue.cpp, MeanReversionStrategy.cpp).
//
// Prediction-market sides ("yes"/"no") are NOT handled here -- those settle
// as a binary payout at market resolution, not a price target/stop, and are
// the outcome resolver's job (Task 8).

#include "services/sandbox/TickTail.h"

#include <QString>
#include <QVector>

namespace openmarketterminal::services::sandbox {

/// Fee schedule in basis points (1 bps = 0.01%). slippage_bps is carried for
/// callers that want to model it on top of a fill/exit price; this file does
/// not apply it itself (try_fill/check_exit return the actual traded price).
struct FeeModel {
    double maker_bps = 0;
    double taker_bps = 0;
    double slippage_bps = 0;
};

struct FillResult {
    bool filled = false;
    double price = 0;
    qint64 ts_ms = 0;
};

// Post-only limit order fill check. ticks must already be restricted to the
// entry window's start by the caller (e.g. ticks_since(path, symbol,
// entry_start_ms)); this function additionally ignores any tick with
// ts_ms > entry_deadline_ms. Processes ticks in the order given (callers
// pass ascending-by-ts_ms vectors) and returns the FIRST tick that
// satisfies the fill condition:
//   buy/long:   tick.price <= limit_price
//   sell/short: tick.price >= limit_price
// Fill price is always limit_price itself (resting maker order gets the
// limit, not the trade print) -- this is the maker side of FeeModel.
// No qualifying tick (or an empty ticks vector) -> filled == false.
FillResult try_fill(const QString& side, double limit_price, const QVector<TickRow>& ticks,
                     qint64 entry_deadline_ms);

struct ExitResult {
    bool exited = false;
    QString reason; // "target" | "stop" | "expiry"
    double price = 0;
    qint64 ts_ms = 0;
};

// Checks ticks (must be ascending by ts_ms) against target/stop/expiry for
// an OPEN position, and returns the FIRST triggering event:
//   long:  stop triggers at tick.price <= stop_price
//          target triggers at tick.price >= target_price
//   short: stop triggers at tick.price >= stop_price
//          target triggers at tick.price <= target_price
// Exit price is always the tick's actual traded price (not target_price /
// stop_price) -- a gap-through tick exits at the worse (or better) print it
// actually traded at, in either direction.
//
// All exits are bounded at expires_at: ticks with ts_ms > expires_at are
// post-expiry prints and are skipped entirely -- they can neither trigger
// target/stop nor set the expiry close price. The position legally dies at
// expires_at; anything that trades after that is not ours (PnL-integrity:
// no harvesting moves from dead positions).
//
// If a single tick's price satisfies both bounds simultaneously, ordering
// within that tick is unknowable, so the STOP wins (conservative). This can
// only happen when stop_price and target_price are not on opposite sides of
// the position in the normal way -- i.e. a misconfigured stop_price >=
// target_price (long) / stop_price <= target_price (short) -- since a
// correctly configured stop/target pair brackets disjoint price ranges and
// no single price can satisfy both.
//
// If no tick triggers and now_ms >= expires_at, reason is "expiry" at the
// price of the LAST tick with ts_ms <= expires_at, with the result's ts_ms
// set to expires_at itself (the moment the position died). If no such
// pre-expiry tick exists, price is 0 -- the caller is responsible for
// treating that as a data gap rather than a real exit at price 0.
// now_ms < expires_at with no tick trigger -> exited == false.
ExitResult check_exit(const QString& side, double target_price, double stop_price, qint64 expires_at,
                       const QVector<TickRow>& ticks, qint64 now_ms);

// Net realized PnL for a closed round trip, all costs included:
//   buy/long:    (exit_price - entry_price) * qty - entry_fee - exit_fee
//   sell/short:  (entry_price - exit_price) * qty - entry_fee - exit_fee
double realized_pnl(const QString& side, double entry_price, double exit_price, double qty, double entry_fee,
                     double exit_fee);

// notional * bps / 10000.
double fee_for(double notional, double bps);

} // namespace openmarketterminal::services::sandbox
