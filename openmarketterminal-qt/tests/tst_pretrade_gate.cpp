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
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), unk, GatePolicy{}).ok); // unknown passes
    }
    void degraded_rejects_unknown_and_ok_pass() {
        GateInputs deg{100.0, QStringLiteral("true"), QStringLiteral("degraded"), true};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0), deg, GatePolicy{}).rule, QStringLiteral("freshness"));
        GateInputs unk{100.0, QStringLiteral("true"), QStringLiteral("unknown"), true};
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), unk, GatePolicy{}).ok); // unknown passes
    }
    void notional_cap_rejects() {
        GatePolicy p; p.max_notional_per_order = 50.0;
        const auto v = evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p); // 100 > 50
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("notional"));
    }
    void position_cap_rejects() {
        GatePolicy p; p.max_position_qty = 0.5;
        const auto v = evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p); // 1 > 0.5
        QVERIFY(!v.ok); QCOMPARE(v.rule, QStringLiteral("position"));
    }
    void venue_not_allowed_rejects_but_missing_skips() {
        GatePolicy p; p.allowed_venues = {QStringLiteral("coinbase_advanced")};
        QCOMPARE(evaluate_pretrade(mk(1.0, 100.0, QStringLiteral("kraken_pro")), fresh_ok(100.0), p).rule,
                 QStringLiteral("venue"));
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), fresh_ok(100.0), p).ok); // no venue on intent -> skip
    }
    void gates_off_allow_bad() {
        GatePolicy p; p.require_cost_gate = false; p.require_freshness_gate = false;
        GateInputs bad{100.0, QStringLiteral("false"), QStringLiteral("degraded"), true};
        QVERIFY(evaluate_pretrade(mk(1.0, 100.0), bad, p).ok); // gates disabled -> allowed
    }
};

QTEST_GUILESS_MAIN(TstPretradeGate)
#include "tst_pretrade_gate.moc"
