#include <QtTest>
#include "services/ai_strategy/DeterministicFloor.h"
#include "services/ai_strategy/Strategy.h"

using namespace openmarketterminal;
using ai_strategy::FloorInputs;
using ai_strategy::FloorPolicy;
using ai_strategy::floor_verdict;
using ai_strategy::intent_reduces_exposure;
using ai_strategy::intent_agrees_with_edge;
using ai_strategy::TradeIntent;

class TstDeterministicFloor : public QObject {
    Q_OBJECT
    static FloorInputs endorsing() { return FloorInputs{true, "pass", "true", "ok"}; }
    static TradeIntent iof(const QString& side, double qty) {
        return TradeIntent{{"symbol", "X-USD"}, {"side", side}, {"quantity", qty}};
    }
  private slots:
    void permits_fully_endorsed() {
        QVERIFY(floor_verdict(endorsing(), FloorPolicy{}).ok);
    }
    void permits_unknown_freshness() {
        FloorInputs in{true, "pass", "true", "unknown"};   // prediction rows carry no freshness
        QVERIFY(floor_verdict(in, FloorPolicy{}).ok);
    }
    void skips_no_edge_signal() {
        FloorInputs in{false, "pass", "true", "ok"};
        const auto v = floor_verdict(in, FloorPolicy{});
        QVERIFY(!v.ok);
        QCOMPARE(v.rule, QStringLiteral("floor"));
        QCOMPARE(v.reason, QStringLiteral("no edge signal"));
    }
    void skips_gate_not_pass() {
        for (const char* g : {"watch", "reject", "protect", ""}) {
            FloorInputs in{true, QString::fromLatin1(g), "true", "ok"};
            const auto v = floor_verdict(in, FloorPolicy{});
            QVERIFY2(!v.ok, g);
            QCOMPARE(v.reason, QStringLiteral("edge gate not pass"));
        }
    }
    void skips_cost_not_affirmatively_clear() {
        for (const char* c : {"unknown", "false"}) {
            FloorInputs in{true, "pass", QString::fromLatin1(c), "ok"};
            const auto v = floor_verdict(in, FloorPolicy{});
            QVERIFY2(!v.ok, c);
            QCOMPARE(v.rule, QStringLiteral("floor"));
        }
    }
    void skips_degraded_freshness() {
        FloorInputs in{true, "pass", "true", "degraded"};
        const auto v = floor_verdict(in, FloorPolicy{});
        QVERIFY(!v.ok);
        QCOMPARE(v.reason, QStringLiteral("stale data"));
    }
    void require_floor_off_permits_all_bad() {
        FloorInputs in{false, "reject", "false", "degraded"};
        QVERIFY(floor_verdict(in, FloorPolicy{false}).ok);   // pass-through when floor disabled
    }
    void reduces_long_via_sell() {
        QVERIFY(intent_reduces_exposure(iof("sell", 5.0), 10.0));    // 10 -> 5
        QVERIFY(intent_reduces_exposure(iof("sell", 10.0), 10.0));   // close
        // Flip-through-zero is a REVERSAL (closes the long, opens an opposite
        // short), not a pure reduction — must NOT be floor-exempt.
        QVERIFY(!intent_reduces_exposure(iof("sell", 15.0), 10.0));  // 10 -> -5: reversal
    }
    void grow_long_is_not_reducing() {
        QVERIFY(!intent_reduces_exposure(iof("buy", 5.0), 10.0));     // 10 -> 15
        QVERIFY(!intent_reduces_exposure(iof("sell", 25.0), 10.0));   // flip-grow: |-15| > 10
    }
    void flat_is_not_reducing() {
        QVERIFY(!intent_reduces_exposure(iof("buy", 5.0), 0.0));
        QVERIFY(!intent_reduces_exposure(iof("sell", 5.0), 0.0));
    }
    void short_cover_reduces_add_does_not() {
        QVERIFY(intent_reduces_exposure(iof("buy", 5.0), -10.0));     // cover: -10 -> -5
        QVERIFY(!intent_reduces_exposure(iof("sell", 5.0), -10.0));   // add short: -10 -> -15
    }
    void long_to_short_reversal_is_not_reduce() {
        // existing +10, sell 15 -> resulting -5: closes the long AND opens a
        // new opposite short. A reversal is not de-risking.
        QVERIFY(!intent_reduces_exposure(iof("sell", 15.0), 10.0));
    }
    void short_to_long_reversal_is_not_reduce() {
        // existing -10, buy 15 -> resulting +5: closes the short AND opens a
        // new opposite long. A reversal is not de-risking.
        QVERIFY(!intent_reduces_exposure(iof("buy", 15.0), -10.0));
    }
    void same_side_reduce_and_close_still_hold() {
        QVERIFY(intent_reduces_exposure(iof("sell", 4.0), 10.0));    // +10 -> +6: partial reduce
        QVERIFY(intent_reduces_exposure(iof("sell", 10.0), 10.0));   // +10 -> 0: full close
        QVERIFY(intent_reduces_exposure(iof("buy", 5.0), -10.0));    // -10 -> -5: short cover/reduce
    }
    void intent_agrees_with_edge_directionality() {
        using ai_strategy::intent_agrees_with_edge;
        // Agreeing directions.
        QVERIFY(intent_agrees_with_edge("buy", "buy"));
        QVERIFY(intent_agrees_with_edge("buy", "long"));
        QVERIFY(intent_agrees_with_edge("sell", "short"));
        QVERIFY(intent_agrees_with_edge("short", "sell"));
        // Disagreeing directions (the F2 bug: long intent on a short edge).
        QVERIFY(!intent_agrees_with_edge("buy", "short"));
        QVERIFY(!intent_agrees_with_edge("buy", "sell"));
        QVERIFY(!intent_agrees_with_edge("sell", "buy"));
        // Neutral/unknown edge side never endorses (fail-closed).
        QVERIFY(!intent_agrees_with_edge("buy", "avoid_buy"));
        QVERIFY(!intent_agrees_with_edge("buy", "hold"));
        QVERIFY(!intent_agrees_with_edge("buy", "flat"));
        QVERIFY(!intent_agrees_with_edge("buy", "yes"));
        QVERIFY(!intent_agrees_with_edge("buy", ""));
        // Neutral intent never endorses.
        QVERIFY(!intent_agrees_with_edge("hold", "buy"));
    }
};

QTEST_MAIN(TstDeterministicFloor)
#include "tst_deterministic_floor.moc"
