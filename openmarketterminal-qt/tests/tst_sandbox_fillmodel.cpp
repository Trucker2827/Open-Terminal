// tst_sandbox_fillmodel.cpp — PaperFillModel: pure fill/exit rules for the
// Strategy Sandbox paper executor (Task 4). Spec-4.3 matrix, one slot per
// case, all hand-computed expectations.
//
// Pure functions over QVector<TickRow> -- no DB, no files, no fixtures
// beyond building small tick vectors inline.

#include <QtTest>

#include "services/sandbox/PaperFillModel.h"

using namespace openmarketterminal::services::sandbox;

namespace {

TickRow tick(double price, qint64 ts_ms, const QString& symbol = QStringLiteral("BTC-USD")) {
    TickRow t;
    t.symbol = symbol;
    t.price = price;
    t.best_bid = price;
    t.best_ask = price;
    t.ts_ms = ts_ms;
    return t;
}

} // namespace

class TstSandboxFillModel : public QObject {
    Q_OBJECT
  private slots:

    // Case 1: buy limit 100, a tick trades exactly at 100.0 -> fills at 100
    // (the resting maker price, not the trade print), maker side.
    void case1_buy_limit_fills_at_limit_price() {
        const QVector<TickRow> ticks = {tick(101.0, 1000), tick(100.0, 2000), tick(99.5, 3000)};
        const FillResult r = try_fill(QStringLiteral("buy"), 100.0, ticks, 5000);
        QVERIFY(r.filled);
        QCOMPARE(r.price, 100.0);
        QCOMPARE(r.ts_ms, qint64(2000));

        // Maker rule, discriminated: a tick strictly THROUGH the limit
        // (99.5 < 100 limit) must still fill at the LIMIT price (100.0),
        // never at the better trade print (99.5) -- resting maker orders
        // don't get price improvement.
        const FillResult deep = try_fill(QStringLiteral("buy"), 100.0, {tick(99.5, 1000)}, 5000);
        QVERIFY(deep.filled);
        QVERIFY(qAbs(deep.price - 100.0) < 1e-9);
    }

    // Case 2: buy limit 100, every tick trades at or above 100.01 -> never
    // qualifies (buy fills at/below limit only) -> NOT filled.
    void case2_buy_limit_not_filled_when_all_ticks_above() {
        const QVector<TickRow> ticks = {tick(100.01, 1000), tick(100.5, 2000), tick(101.0, 3000)};
        const FillResult r = try_fill(QStringLiteral("buy"), 100.0, ticks, 5000);
        QVERIFY(!r.filled);
    }

    // Case 3: a tick at 99.9 (which would otherwise qualify a buy-limit-100)
    // arrives strictly after entry_deadline_ms -> excluded -> NOT filled.
    void case3_tick_after_entry_deadline_not_filled() {
        const QVector<TickRow> ticks = {tick(99.9, 6000)};
        const FillResult r = try_fill(QStringLiteral("buy"), 100.0, ticks, 5000);
        QVERIFY(!r.filled);
    }

    // Case 4: long open entry 100, target 105, stop 97. Tick sequence 103
    // (no bound crossed), then 105.2 (crosses target). Exit price is the
    // tick's ACTUAL traded price (105.2), not the target price (105).
    void case4_long_target_exit_uses_actual_tick_price() {
        const QVector<TickRow> ticks = {tick(103.0, 1000), tick(105.2, 2000)};
        const ExitResult r = check_exit(QStringLiteral("long"), 105.0, 97.0, 999999, ticks, 1500);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("target"));
        QVERIFY(qAbs(r.price - 105.2) < 1e-9);
        QCOMPARE(r.ts_ms, qint64(2000));
    }

    // Case 5: gap-through in both directions -- exit always uses the tick's
    // actual traded price, never the configured stop/target price.
    // 5a: stop gap-through: ticks 99 (no cross), 92 (blows through stop 97)
    //     -> reason "stop", price 92 (worse than the stop level, not 97).
    // 5b: target gap-through: ticks 103 (no cross), 108 (blows through
    //     target 105) -> reason "target", price 108 (not clamped to 105).
    void case5_gap_through_ticks_exit_at_actual_price_both_directions() {
        const QVector<TickRow> stop_gap = {tick(99.0, 1000), tick(92.0, 2000)};
        const ExitResult stop_r = check_exit(QStringLiteral("long"), 105.0, 97.0, 999999, stop_gap, 1500);
        QVERIFY(stop_r.exited);
        QCOMPARE(stop_r.reason, QStringLiteral("stop"));
        QVERIFY(qAbs(stop_r.price - 92.0) < 1e-9);

        const QVector<TickRow> target_gap = {tick(103.0, 1000), tick(108.0, 2000)};
        const ExitResult target_r = check_exit(QStringLiteral("long"), 105.0, 97.0, 999999, target_gap, 1500);
        QVERIFY(target_r.exited);
        QCOMPARE(target_r.reason, QStringLiteral("target"));
        QVERIFY(qAbs(target_r.price - 108.0) < 1e-9);
    }

    // Case 6: the stop-wins rule. Only meaningful when a SINGLE tick's price
    // satisfies both bounds simultaneously, which requires a misconfigured
    // stop_price >= target_price (long): here stop == target == 100, tick
    // trades at exactly 100 -> both bounds are satisfied by the same tick,
    // ordering within the tick is unknowable -> conservative stop wins.
    void case6_single_tick_satisfies_both_bounds_stop_wins() {
        const QVector<TickRow> ticks = {tick(100.0, 1000)};
        const ExitResult r = check_exit(QStringLiteral("long"), 100.0, 100.0, 999999, ticks, 1500);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("stop"));
        QVERIFY(qAbs(r.price - 100.0) < 1e-9);
    }

    // Case 7: no target/stop hit by any tick; now_ms has passed expires_at
    // -> reason "expiry" at the LAST pre-expiry tick's price (101 @2400),
    // not the first or any intermediate one, with ts_ms == expires_at (the
    // position legally dies at expiry, not at observation time).
    void case7_expiry_uses_last_tick_price() {
        const QVector<TickRow> ticks = {tick(102.0, 1000), tick(99.0, 2000), tick(101.0, 2400)};
        const ExitResult r = check_exit(QStringLiteral("long"), 200.0, 50.0, 2500, ticks, 3000);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("expiry"));
        QVERIFY(qAbs(r.price - 101.0) < 1e-9);
        QCOMPARE(r.ts_ms, qint64(2500));
    }

    // Regression (expiry bound, reviewer-mandated): a post-expiry tick at a
    // target-crossing price must NOT trigger a target exit -- the position
    // died at expires_at, and prints after that are not ours to trade.
    // Without the ts_ms <= expires_at bound in check_exit's tick loop, this
    // returned reason "target" @106 (PnL-integrity bias: free upside from
    // dead positions). expires 2000, only tick @2500 at 106 (crosses target
    // 105): result must be expiry, and with no pre-expiry tick at all,
    // price 0 (data-gap sentinel) with ts == expires_at.
    void case7a_post_expiry_tick_cannot_trigger_target() {
        const QVector<TickRow> ticks = {tick(106.0, 2500)};
        const ExitResult r = check_exit(QStringLiteral("long"), 105.0, 97.0, 2000, ticks, 3000);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("expiry"));
        QCOMPARE(r.price, 0.0);
        QCOMPARE(r.ts_ms, qint64(2000));
    }

    // Regression (expiry bound, reviewer-mandated): the expiry close price
    // is the last tick AT OR BEFORE expires_at, ignoring later prints
    // entirely. Ticks 101 @1500 and 108 @2600 (post-expiry, and also
    // target-crossing), expires 2000, now 3000 -> expiry at 101 with
    // ts == expires_at (not 108, and not a target exit).
    void case7b_expiry_price_ignores_post_expiry_tick() {
        const QVector<TickRow> ticks = {tick(101.0, 1500), tick(108.0, 2600)};
        const ExitResult r = check_exit(QStringLiteral("long"), 105.0, 97.0, 2000, ticks, 3000);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("expiry"));
        QVERIFY(qAbs(r.price - 101.0) < 1e-9);
        QCOMPARE(r.ts_ms, qint64(2000));
    }

    // Case 8: expiry with zero ticks available at all -- still exits with
    // reason "expiry" but price 0 (caller's job to recognize the data gap),
    // timestamped at expires_at (when the position legally died).
    void case8_expiry_with_zero_ticks_reports_price_zero() {
        const QVector<TickRow> ticks;
        const ExitResult r = check_exit(QStringLiteral("long"), 200.0, 50.0, 2500, ticks, 3000);
        QVERIFY(r.exited);
        QCOMPARE(r.reason, QStringLiteral("expiry"));
        QCOMPARE(r.price, 0.0);
        QCOMPARE(r.ts_ms, qint64(2500));
    }

    // Case 9: no exit at all -- every tick stays strictly within
    // (stop, target), and now_ms has not yet reached expires_at.
    void case9_no_exit_when_within_bounds_and_before_expiry() {
        const QVector<TickRow> ticks = {tick(100.5, 1000), tick(101.0, 2000), tick(99.5, 3000)};
        const ExitResult r = check_exit(QStringLiteral("long"), 105.0, 97.0, 5000, ticks, 3000);
        QVERIFY(!r.exited);
    }

    // Case 10: realized_pnl, long and short mirrors, both exactly 2.40.
    // Long:  (105 - 100) * 0.5 - 0.05 - 0.05 = 2.5 - 0.10 = 2.40
    // Short: (100 - 95)  * 0.5 - 0.05 - 0.05 = 2.5 - 0.10 = 2.40
    // Also exercises fee_for on a clean round number: 10000 * 5bps / 10000
    // = 5.0 exactly.
    void case10_realized_pnl_long_and_short_mirror() {
        const double long_pnl = realized_pnl(QStringLiteral("long"), 100.0, 105.0, 0.5, 0.05, 0.05);
        QVERIFY(qAbs(long_pnl - 2.40) < 1e-9);

        const double short_pnl = realized_pnl(QStringLiteral("short"), 100.0, 95.0, 0.5, 0.05, 0.05);
        QVERIFY(qAbs(short_pnl - 2.40) < 1e-9);

        QVERIFY(qAbs(fee_for(10000.0, 5.0) - 5.0) < 1e-9);
    }

    // Case 11: short side mirrors on both try_fill and check_exit.
    // try_fill: sell limit 100 fills at/above limit (tick 100.0 qualifies;
    // all-below-100 ticks do not). check_exit: for a short, stop is ABOVE
    // entry and target is BELOW entry -- entry 100, target 95, stop 103;
    // tick sequence 97 (no cross) then 94.8 (crosses target, below 95) ->
    // reason "target" at actual price 94.8. Then separately a stop mirror:
    // ticks 101 (no cross) then 103.7 (crosses stop, at/above 103) ->
    // reason "stop" at actual price 103.7.
    void case11_short_side_mirrors_fill_and_exit() {
        const QVector<TickRow> fill_ticks = {tick(99.0, 1000), tick(100.0, 2000)};
        const FillResult fill_r = try_fill(QStringLiteral("sell"), 100.0, fill_ticks, 5000);
        QVERIFY(fill_r.filled);
        QCOMPARE(fill_r.price, 100.0);
        QCOMPARE(fill_r.ts_ms, qint64(2000));

        // Maker rule mirror, discriminated: a tick strictly THROUGH the
        // sell limit (100.5 > 100 limit) still fills at the LIMIT (100.0),
        // never at the better trade print (100.5).
        const FillResult deep = try_fill(QStringLiteral("sell"), 100.0, {tick(100.5, 1000)}, 5000);
        QVERIFY(deep.filled);
        QVERIFY(qAbs(deep.price - 100.0) < 1e-9);

        const QVector<TickRow> not_filled_ticks = {tick(99.99, 1000), tick(98.0, 2000)};
        const FillResult not_filled = try_fill(QStringLiteral("sell"), 100.0, not_filled_ticks, 5000);
        QVERIFY(!not_filled.filled);

        const QVector<TickRow> target_ticks = {tick(97.0, 1000), tick(94.8, 2000)};
        const ExitResult target_r = check_exit(QStringLiteral("short"), 95.0, 103.0, 999999, target_ticks, 1500);
        QVERIFY(target_r.exited);
        QCOMPARE(target_r.reason, QStringLiteral("target"));
        QVERIFY(qAbs(target_r.price - 94.8) < 1e-9);

        const QVector<TickRow> stop_ticks = {tick(101.0, 1000), tick(103.7, 2000)};
        const ExitResult stop_r = check_exit(QStringLiteral("short"), 95.0, 103.0, 999999, stop_ticks, 1500);
        QVERIFY(stop_r.exited);
        QCOMPARE(stop_r.reason, QStringLiteral("stop"));
        QVERIFY(qAbs(stop_r.price - 103.7) < 1e-9);
    }

    // Honest maker fill: a mere touch of the limit does NOT fill when a
    // through-margin is required (queue / adverse selection); the price must
    // trade strictly through the limit by the margin. through_bps == 0 must
    // reproduce try_fill's optimistic touch-fills behaviour.
    void maker_fill_requires_trade_through_not_touch() {
        // buy limit 100, 5 bps through => threshold 99.95. A touch at 100.0
        // and a shallow 99.97 both fail; 99.9 (below threshold) fills at 100.
        const QVector<TickRow> touch = {tick(100.0, 1000), tick(99.97, 2000)};
        QVERIFY(!try_maker_fill(QStringLiteral("buy"), 100.0, touch, 5000, 5.0).filled);

        const QVector<TickRow> through = {tick(100.0, 1000), tick(99.9, 2000)};
        const FillResult f = try_maker_fill(QStringLiteral("buy"), 100.0, through, 5000, 5.0);
        QVERIFY(f.filled);
        QVERIFY(qAbs(f.price - 100.0) < 1e-9);   // maker earns the limit, no improvement
        QCOMPARE(f.ts_ms, qint64(2000));

        // sell limit 100, 5 bps through => threshold 100.05; a touch at 100.0
        // fails, 100.1 fills.
        QVERIFY(!try_maker_fill(QStringLiteral("sell"), 100.0, {tick(100.0, 1000)}, 5000, 5.0).filled);
        QVERIFY(try_maker_fill(QStringLiteral("sell"), 100.0, {tick(100.1, 1000)}, 5000, 5.0).filled);

        // through_bps == 0 reproduces the optimistic touch-fills behaviour.
        QVERIFY(try_maker_fill(QStringLiteral("buy"), 100.0, {tick(100.0, 1000)}, 5000, 0.0).filled);
    }

    // Taker crosses the half-spread AND pays slippage, both adverse: a buyer
    // pays up, a seller receives down, by (half_spread + slippage) bps.
    void taker_price_crosses_spread_and_slippage() {
        // 2 bps half-spread + 1 bps slippage = 3 bps = 0.03% adverse.
        const double buy = effective_taker_price(QStringLiteral("buy"), 100.0, 2.0, 1.0);
        QVERIFY(qAbs(buy - 100.03) < 1e-9);      // pays UP
        const double sell = effective_taker_price(QStringLiteral("sell"), 100.0, 2.0, 1.0);
        QVERIFY(qAbs(sell - 99.97) < 1e-9);      // receives DOWN
        // long/short mirror buy/sell.
        QVERIFY(qAbs(effective_taker_price(QStringLiteral("long"), 100.0, 2.0, 1.0) - 100.03) < 1e-9);
        QVERIFY(qAbs(effective_taker_price(QStringLiteral("short"), 100.0, 2.0, 1.0) - 99.97) < 1e-9);
        // Zero costs => reference price unchanged.
        QVERIFY(qAbs(effective_taker_price(QStringLiteral("buy"), 100.0, 0.0, 0.0) - 100.0) < 1e-9);
    }
};

QTEST_GUILESS_MAIN(TstSandboxFillModel)
#include "tst_sandbox_fillmodel.moc"
