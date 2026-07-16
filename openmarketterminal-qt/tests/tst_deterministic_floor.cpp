#include <QtTest>
#include "services/ai_strategy/DeterministicFloor.h"
#include "services/ai_strategy/Strategy.h"

using namespace openmarketterminal;
using ai_strategy::FloorInputs;
using ai_strategy::FloorPolicy;
using ai_strategy::floor_verdict;
using ai_strategy::intent_reduces_exposure;
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
        QVERIFY(intent_reduces_exposure(iof("sell", 15.0), 10.0));   // flip-reduce: |-5| <= 10
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
};

QTEST_MAIN(TstDeterministicFloor)
#include "tst_deterministic_floor.moc"
