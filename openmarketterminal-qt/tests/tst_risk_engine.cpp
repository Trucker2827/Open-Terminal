// Coverage for the alpha-arena RiskEngine (services/alpha_arena/RiskEngine) —
// the server-side, fail-closed gate between a model's ProposedAction and the
// OrderRouter, plus the per-agent circuit breaker (drawdown / parse-failure /
// risk-reject). Pure functions, no I/O; an error here lets a model place a
// trade that violates the leverage / risk / liquidation rules, or fails to
// halt a blown-up agent. Untested before this.

#include "services/alpha_arena/RiskEngine.h"
#include "services/alpha_arena/AlphaArenaSchema.h"

#include <QtTest>

using namespace openmarketterminal::services::alpha_arena;

namespace {

// A long entry that passes EVERY check (baseline; perturbed per-test to trip
// one rule at a time). mark=100, equity=10k, leverage 5 → liq buffer 19%
// (>= 15%), risk = |100-99|*10 = 10 (<= 2% of 10k = 200).
ProposedAction goodLongEntry() {
    ProposedAction a;
    a.signal = Signal::BuyToEnter;
    a.coin = QStringLiteral("BTC");
    a.quantity = 10.0;
    a.leverage = 5;
    a.profit_target = 110.0;  // above mark (long)
    a.stop_loss = 99.0;       // below mark (long)
    a.risk_usd = 0.0;         // 0 => skip drift check
    return a;
}

AgentRiskState baseAgent() {
    AgentRiskState s;
    s.equity = 10000.0;
    s.existing_qty_in_coin = 0.0;
    s.last_entry_utc_ms_on_coin = 0;
    s.now_utc_ms = 0;
    s.maintenance_margin_frac = 0.05;
    s.mark_price = 100.0;
    return s;
}

bool isAccept(const RiskVerdict& v) { return v.outcome == RiskVerdict::Outcome::Accept; }
bool isReject(const RiskVerdict& v) { return v.outcome == RiskVerdict::Outcome::Reject; }

}  // namespace

class TestRiskEngine : public QObject {
    Q_OBJECT
  private slots:
    // helpers
    void recomputeRiskUsd();
    void liquidationPriceFormula();
    // evaluate_action
    void baselineLongAccepts();
    void shortEntryAccepts();
    void holdAlwaysAccepts();
    void closeNeedsPosition();
    void onePositionPerCoin();
    void leverageBounds();
    void stopsMustBeOnRightSide();
    void liquidationBufferFloor();
    void riskPerTradeCap();
    void monkCooldownAndNotional();
    void riskUsdDriftAmends();
    // advance_circuit (drawdown + breaker)
    void drawdownTripsBreaker();
    void maxDrawdownIsMonotonic();
    void parseFailuresTripAfterThree();
    void riskRejectsTripAfterThree();
    void cleanTickResetsCounters();
    void openCircuitLatches();
};

void TestRiskEngine::recomputeRiskUsd() {
    ProposedAction a = goodLongEntry();  // stop 99, qty 10
    QCOMPARE(recompute_risk_usd(a, 100.0), 10.0);          // |100-99|*10
    QCOMPARE(recompute_risk_usd(a, 0.0), 0.0);             // bad entry
    a.stop_loss = 0.0;
    QCOMPARE(recompute_risk_usd(a, 100.0), 0.0);           // no stop
    a = goodLongEntry();
    a.quantity = 0.0;
    QCOMPARE(recompute_risk_usd(a, 100.0), 0.0);           // no qty
}

void TestRiskEngine::liquidationPriceFormula() {
    // long: entry*(1 - (1/lev)*(1-mm)); short: entry*(1 + ...).
    QCOMPARE(estimated_liquidation_price(100.0, 5, true, 0.05), 100.0 * (1.0 - 0.19));
    QCOMPARE(estimated_liquidation_price(100.0, 5, false, 0.05), 100.0 * (1.0 + 0.19));
    QCOMPARE(estimated_liquidation_price(0.0, 5, true, 0.05), 0.0);    // bad entry
    QCOMPARE(estimated_liquidation_price(100.0, 0, true, 0.05), 0.0);  // bad leverage
}

void TestRiskEngine::baselineLongAccepts() {
    QVERIFY(isAccept(evaluate_action(goodLongEntry(), baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::shortEntryAccepts() {
    ProposedAction a = goodLongEntry();
    a.signal = Signal::SellToEnter;
    a.stop_loss = 101.0;     // above mark (short)
    a.profit_target = 90.0;  // below mark (short)
    QVERIFY(isAccept(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::holdAlwaysAccepts() {
    ProposedAction a;  // default signal = Hold
    QVERIFY(isAccept(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::closeNeedsPosition() {
    ProposedAction a;
    a.signal = Signal::Close;
    AgentRiskState flat = baseAgent();  // existing_qty 0
    QVERIFY(isReject(evaluate_action(a, flat, CompetitionMode::Baseline)));
    AgentRiskState held = baseAgent();
    held.existing_qty_in_coin = 3.0;
    QVERIFY(isAccept(evaluate_action(a, held, CompetitionMode::Baseline)));
}

void TestRiskEngine::onePositionPerCoin() {
    AgentRiskState held = baseAgent();
    held.existing_qty_in_coin = 1.0;
    QVERIFY(isReject(evaluate_action(goodLongEntry(), held, CompetitionMode::Baseline)));
}

void TestRiskEngine::leverageBounds() {
    ProposedAction a = goodLongEntry();
    a.leverage = 21;
    QVERIFY(isReject(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
    a.leverage = 0;
    QVERIFY(isReject(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::stopsMustBeOnRightSide() {
    AgentRiskState agent = baseAgent();
    ProposedAction a = goodLongEntry();
    a.stop_loss = 101.0;  // long stop above mark — wrong
    QVERIFY(isReject(evaluate_action(a, agent, CompetitionMode::Baseline)));
    a = goodLongEntry();
    a.profit_target = 90.0;  // long target below mark — wrong
    QVERIFY(isReject(evaluate_action(a, agent, CompetitionMode::Baseline)));
}

void TestRiskEngine::liquidationBufferFloor() {
    // Leverage 10 → buffer (1/10)*0.95 = 9.5% < 15% floor → reject.
    ProposedAction a = goodLongEntry();
    a.leverage = 10;
    QVERIFY(isReject(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::riskPerTradeCap() {
    // |100-90|*30 = 300 > 2% of 10k = 200 → reject. (leverage 5 keeps buffer ok.)
    ProposedAction a = goodLongEntry();
    a.stop_loss = 90.0;
    a.quantity = 30.0;
    QVERIFY(isReject(evaluate_action(a, baseAgent(), CompetitionMode::Baseline)));
}

void TestRiskEngine::monkCooldownAndNotional() {
    // 30-min cooldown: last entry 10 min ago → reject in Monk, accept in Baseline.
    AgentRiskState agent = baseAgent();
    agent.now_utc_ms = 10LL * 30LL * 60LL * 1000LL;  // arbitrary "now" well past 30m
    agent.last_entry_utc_ms_on_coin = agent.now_utc_ms - 10LL * 60LL * 1000LL;  // 10 min ago
    QVERIFY(isReject(evaluate_action(goodLongEntry(), agent, CompetitionMode::Monk)));
    QVERIFY(isAccept(evaluate_action(goodLongEntry(), agent, CompetitionMode::Baseline)));

    // Notional 26*100 = 2600 > 25% of 10k = 2500 → Monk reject; risk = 1*26 = 26
    // stays under the 2% cap so it's the notional rule (not the risk rule) firing.
    AgentRiskState fresh = baseAgent();  // no prior entry
    ProposedAction big = goodLongEntry();
    big.quantity = 26.0;  // stop_loss stays 99 → risk 26
    QVERIFY(isReject(evaluate_action(big, fresh, CompetitionMode::Monk)));
    QVERIFY(isAccept(evaluate_action(big, fresh, CompetitionMode::Baseline)));
}

void TestRiskEngine::riskUsdDriftAmends() {
    // Model claims risk_usd 100 but server recomputes 10 → drift 90% > 5% → Amend
    // with the corrected value, NOT reject.
    ProposedAction a = goodLongEntry();
    a.risk_usd = 100.0;
    const RiskVerdict v = evaluate_action(a, baseAgent(), CompetitionMode::Baseline);
    QCOMPARE(v.outcome, RiskVerdict::Outcome::Amend);
    QVERIFY(v.amended.has_value());
    QCOMPARE(v.amended->risk_usd, 10.0);
}

void TestRiskEngine::drawdownTripsBreaker() {
    CircuitState fresh;  // closed, all counters 0
    QVERIFY(advance_circuit(fresh, false, false, 0.50).open);   // >= 50% → open
    QVERIFY(!advance_circuit(fresh, false, false, 0.49).open);  // below → stays closed
}

void TestRiskEngine::maxDrawdownIsMonotonic() {
    CircuitState prev;
    prev.max_drawdown_frac = 0.40;
    QCOMPARE(advance_circuit(prev, false, false, 0.10).max_drawdown_frac, 0.40);  // keeps the max
    QCOMPARE(advance_circuit(prev, false, false, 0.45).max_drawdown_frac, 0.45);  // raises it
}

void TestRiskEngine::parseFailuresTripAfterThree() {
    CircuitState prev;
    prev.consecutive_parse_failures = 2;  // third in a row trips (limit 3)
    const CircuitState next = advance_circuit(prev, true, false, 0.0);
    QCOMPARE(next.consecutive_parse_failures, 3);
    QVERIFY(next.open);
}

void TestRiskEngine::riskRejectsTripAfterThree() {
    CircuitState prev;
    prev.consecutive_risk_rejects = 2;
    const CircuitState next = advance_circuit(prev, false, true, 0.0);
    QCOMPARE(next.consecutive_risk_rejects, 3);
    QVERIFY(next.open);
}

void TestRiskEngine::cleanTickResetsCounters() {
    CircuitState prev;
    prev.consecutive_parse_failures = 2;
    prev.consecutive_risk_rejects = 2;
    const CircuitState next = advance_circuit(prev, false, false, 0.0);
    QCOMPARE(next.consecutive_parse_failures, 0);
    QCOMPARE(next.consecutive_risk_rejects, 0);
    QVERIFY(!next.open);
}

void TestRiskEngine::openCircuitLatches() {
    // Once open, stays open even on a perfectly clean tick (manual resume only).
    CircuitState prev;
    prev.open = true;
    QVERIFY(advance_circuit(prev, false, false, 0.0).open);
}

QTEST_APPLESS_MAIN(TestRiskEngine)
#include "tst_risk_engine.moc"
