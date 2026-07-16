#include <QtTest>
#include "services/ai_strategy/DeterministicFloor.h"

using namespace openmarketterminal;
using ai_strategy::FloorInputs;
using ai_strategy::FloorPolicy;
using ai_strategy::floor_verdict;

class TstDeterministicFloor : public QObject {
    Q_OBJECT
    static FloorInputs endorsing() { return FloorInputs{true, "pass", "true", "ok"}; }
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
};

QTEST_MAIN(TstDeterministicFloor)
#include "tst_deterministic_floor.moc"
