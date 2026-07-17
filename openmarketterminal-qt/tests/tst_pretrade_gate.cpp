// tst_pretrade_gate.cpp — pure pre-trade guardrail. No DB, no settings.
#include "services/ai_strategy/PretradeGate.h"
#include "services/ai_strategy/Strategy.h"

#include <QtTest/QtTest>

using namespace openmarketterminal::ai_strategy;

namespace {
TradeIntent mk(double qty, double limit = 0.0, const QString& venue = {}) {
    TradeIntent t{{"symbol", "BTC-USD"}, {"side", "buy"}, {"quantity", qty}};
    if (limit > 0) t.insert("limit_price", limit);
    if (!venue.isEmpty()) t.insert("venue", venue);
    return t;
}
GateInputs fresh_ok(double price) { return GateInputs{price, QStringLiteral("true"), QStringLiteral("ok"), true}; }
TradeIntent intent_of(const QString& side, double qty) {
    return TradeIntent{{"symbol", "X-USD"}, {"side", side}, {"quantity", qty},
                       {"order_type", "limit"}, {"limit_price", 100.0}};
}
} // namespace

class TstPretradeGate : public QObject {
    Q_OBJECT
  private slots:
    void all_good_allows() {
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), GatePolicy{}).ok);
    }
    void non_positive_qty_rejects() {
        const auto v = evaluate_pretrade(mk(0.0, 100.0), fresh_ok(100.0), GatePolicy{});
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("position"));
    }
    void no_price_rejects() {
        GateInputs in{0.0, QStringLiteral("true"), QStringLiteral("ok"), true};
        const auto v = evaluate_pretrade(mk(1.0), in, GatePolicy{});
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("cost"));
    }
    void below_cost_rejects_unknown_and_true_pass() {
        GateInputs bad{100.0, QStringLiteral("false"), QStringLiteral("ok"), true};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), bad, GatePolicy{}).rule, QStringLiteral("cost"));
        GateInputs unk{100.0, QStringLiteral("unknown"), QStringLiteral("ok"), false};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), unk, GatePolicy{}).rule, QStringLiteral("cost"));
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), GatePolicy{}).ok);
    }
    void degraded_rejects_unknown_and_ok_pass() {
        GateInputs deg{100.0, QStringLiteral("true"), QStringLiteral("degraded"), true};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), deg, GatePolicy{}).rule, QStringLiteral("freshness"));
        GateInputs unk{100.0, QStringLiteral("true"), QStringLiteral("unknown"), true};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), unk, GatePolicy{}).rule, QStringLiteral("freshness"));
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), GatePolicy{}).ok);
    }
    void notional_cap_rejects() {
        GatePolicy p; p.max_notional_per_order = 50.0;
        const auto v = evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p); // 100 > 50
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("notional"));
    }
    void notional_cap_exempts_de_risking() {
        GatePolicy p;
        p.max_notional_per_order = 500.0;
        // GROWING order over the cap -> rejected {notional} (regression guard on the cap itself).
        {
            GateInputs in = fresh_ok(100.0);
            in.existing_net_qty = 0.0;  // flat -> a buy grows exposure
            const TradeIntent buy{{"symbol", "X"}, {"side", "buy"}, {"quantity", 10.0}};  // notional 1000 > 500
            const auto v = evaluate_pretrade(buy, in, p);
            QVERIFY(!v.ok);
            QCOMPARE(v.rule, QStringLiteral("notional"));
        }
        // REDUCING order over the cap -> PASSES (de-risking never blocked).
        {
            GateInputs in = fresh_ok(100.0);
            in.existing_net_qty = 100.0;  // long 100
            const TradeIntent exit{{"symbol", "X"}, {"side", "sell"}, {"quantity", 100.0}};  // notional 10000 >> 500, full exit
            const auto v = evaluate_pretrade(exit, in, p);
            QVERIFY2(v.ok, "a full de-risking exit must not be notional-capped");
        }
    }
    void position_cap_rejects() {
        GatePolicy p; p.max_position_qty = 0.5;
        const auto v = evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p); // 1 > 0.5
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("position"));
    }
    void venue_policy_rejects_missing_and_unlisted_but_allows_normalized_match() {
        GatePolicy p; p.allowed_venues = {QStringLiteral(" Coinbase_Advanced ")};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0, QStringLiteral("kraken_pro")), fresh_ok(100.0), p).rule,
                 QStringLiteral("venue"));
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p).rule, QStringLiteral("venue"));
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0, QStringLiteral("COINBASE_ADVANCED")), fresh_ok(100.0), p).ok);
    }
    void gates_off_allow_bad() {
        GatePolicy p; p.require_cost_gate = false; p.require_freshness_gate = false;
        GateInputs bad{100.0, QStringLiteral("false"), QStringLiteral("degraded"), true};
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), bad, p).ok); // gates disabled -> allowed
    }
    void evidence_gates_do_not_block_a_pure_reduction() {
        GateInputs in{100.0, QStringLiteral("unknown"), QStringLiteral("unknown"), false};
        in.existing_net_qty = 5.0;
        QVERIFY(evaluate_pretrade(intent_of(QStringLiteral("sell"), 5.0), in, GatePolicy{}).ok);

        // A reversal is a new exposure, not a de-risking exit, so it remains blocked.
        QCOMPARE(evaluate_pretrade(intent_of(QStringLiteral("sell"), 7.0), in, GatePolicy{}).rule,
                 QStringLiteral("cost"));
    }
    void cumulative_cap_blocks_growing_over_cap() {
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 8.0;
        GatePolicy p; p.max_position_qty = 10.0;
        GateVerdict v = evaluate_pretrade(intent_of("buy", 5.0), in, p);  // 8+5=13 > 10 and > 8
        QVERIFY(!v.ok);
        QCOMPARE(v.rule, QStringLiteral("position"));
    }
    void cumulative_cap_allows_reduce_while_over_cap() {
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 15.0;  // over cap 10
        GatePolicy p; p.max_position_qty = 10.0;
        // 15-3=12, still > cap 10 BUT |12| <= |15| (reducing) -> allow. This is the case that
        // exercises the increase-only clause: |resulting|>cap yet still permitted because it shrinks.
        GateVerdict v = evaluate_pretrade(intent_of("sell", 3.0), in, p);
        QVERIFY(v.ok);
    }
    void cumulative_cap_allows_flip_that_reduces() {
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 10.0;
        GatePolicy p; p.max_position_qty = 10.0;
        GateVerdict v = evaluate_pretrade(intent_of("sell", 13.0), in, p);  // 10-13=-3, |−3| <= |10| -> allow
        QVERIFY(v.ok);
    }
    void cumulative_cap_blocks_flip_that_grows() {
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 10.0;
        GatePolicy p; p.max_position_qty = 10.0;
        GateVerdict v = evaluate_pretrade(intent_of("sell", 25.0), in, p);  // 10-25=-15, |−15|>10 and >|10| -> reject
        QVERIFY(!v.ok);
        QCOMPARE(v.rule, QStringLiteral("position"));
    }
    void cumulative_cap_collapses_to_per_intent_when_flat() {
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 0.0;
        GatePolicy p; p.max_position_qty = 10.0;
        QVERIFY(!evaluate_pretrade(intent_of("buy", 15.0), in, p).ok);  // 0+15=15 > 10 -> reject (old rule)
        QVERIFY(evaluate_pretrade(intent_of("buy", 5.0), in, p).ok);    // 0+5=5 <= 10 -> allow
    }
    void aggregate_cap_blocks_growing_over_cap() {
        GateInputs in = fresh_ok(100.0); in.aggregate_net_qty = 8.0;
        GatePolicy p; p.max_aggregate_position_qty = 10.0;
        GateVerdict v = evaluate_pretrade(intent_of("buy", 5.0), in, p);  // 8+5=13 > 10 and > 8
        QVERIFY(!v.ok);
        QCOMPARE(v.rule, QStringLiteral("aggregate"));
    }
    void aggregate_cap_allows_reduce_over_cap() {
        GateInputs in = fresh_ok(100.0); in.aggregate_net_qty = 15.0;   // over cap 10
        GatePolicy p; p.max_aggregate_position_qty = 10.0;
        // 15-3=12, still > cap but |12| <= |15| (reducing) -> allow.
        QVERIFY(evaluate_pretrade(intent_of("sell", 3.0), in, p).ok);
    }
    void aggregate_cap_flat_and_small_add() {
        GatePolicy p; p.max_aggregate_position_qty = 10.0;
        GateInputs flat = fresh_ok(100.0); flat.aggregate_net_qty = 0.0;
        QVERIFY(!evaluate_pretrade(intent_of("buy", 15.0), flat, p).ok);   // 0+15=15 > 10 -> reject
        GateInputs small = fresh_ok(100.0); small.aggregate_net_qty = 8.0;
        QVERIFY(evaluate_pretrade(intent_of("buy", 1.0), small, p).ok);    // 9 <= 10 -> pass
    }
    void aggregate_cap_fires_when_handler_position_is_fine() {
        // This handler's own position is tiny, but the cross-handler aggregate is near cap.
        GateInputs in = fresh_ok(100.0); in.existing_net_qty = 2.0; in.aggregate_net_qty = 9.0;
        GatePolicy p; p.max_position_qty = 100.0; p.max_aggregate_position_qty = 10.0;  // per-handler won't fire
        GateVerdict v = evaluate_pretrade(intent_of("buy", 5.0), in, p);   // agg 9+5=14 > 10 -> reject{aggregate}
        QVERIFY(!v.ok);
        QCOMPARE(v.rule, QStringLiteral("aggregate"));
    }
    void aggregate_cap_off_by_default() {
        GateInputs in = fresh_ok(100.0); in.aggregate_net_qty = 1000.0;
        GatePolicy p;  // max_aggregate_position_qty defaults 0 -> no cap
        QVERIFY(evaluate_pretrade(intent_of("buy", 1000.0), in, p).ok);
    }
};

QTEST_GUILESS_MAIN(TstPretradeGate)
#include "tst_pretrade_gate.moc"
