// tst_strategy_loop.cpp — StrategyRunner harness (Paper Strategy-Loop, Task 1).
//
// Pure-fake unit tests for the loop wiring: snapshot build, prepare→submit
// pipeline, rejection accounting, and clean kill-switch halt. No real
// execution, no DB needed — the kill switch reads false from an empty DB under
// a QTemporaryDir HOME (set so a developer's real cli.kill_switch can't trip
// these tests).
#include <QtTest>
#include <QQueue>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include "services/ai_decision/DecisionContext.h"
#include "services/ai_ledger/AiLedger.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/Strategy.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "services/ai_strategy/TypedAction.h"
#include "storage/repositories/AiFillRepository.h"
#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"

using namespace openmarketterminal;
using ai_strategy::ActionChoice;
using ai_strategy::ActionType;
using ai_strategy::LlmStrategy;
using ai_strategy::MarketSnapshot;
using ai_strategy::MeanReversionStrategy;
using ai_strategy::RunConfig;
using ai_strategy::RunSummary;
using ai_strategy::Strategy;
using ai_strategy::StrategyRunner;
using ai_strategy::ToolCaller;
using ai_strategy::TradeIntent;
using mcp::ToolResult;

// ── Fakes ────────────────────────────────────────────────────────────────────

class FakeToolCaller : public ToolCaller {
  public:
    QMap<QString, QQueue<ToolResult>> scripted;       ///< tool name → queued results.
    QVector<QPair<QString, QJsonObject>> calls;        ///< every call, in order.

    void enqueue(const QString& tool, const ToolResult& r) { scripted[tool].enqueue(r); }

    ToolResult call(const QString& name, const QJsonObject& args) override {
        calls.append({name, args});
        auto it = scripted.find(name);
        if (it != scripted.end() && !it->isEmpty())
            return it->dequeue();
        return ToolResult::ok_data(QJsonObject{}); // default success
    }

    // Count calls to a given tool.
    int count(const QString& name) const {
        int n = 0;
        for (const auto& c : calls)
            if (c.first == name)
                ++n;
        return n;
    }
};

class FakeStrategy : public Strategy {
  public:
    QStringList universe_ = {QStringLiteral("AAA")};
    QVector<TradeIntent> intents_;
    QVector<QJsonObject> fills; ///< submit results passed to on_fill.

    QString name() const override { return QStringLiteral("fake"); }
    QStringList universe() const override { return universe_; }
    QVector<TradeIntent> propose(const MarketSnapshot&) override { return intents_; }
    void on_fill(const TradeIntent&, const QJsonObject& submit_result) override {
        fills.append(submit_result);
    }
};

// Result-shape helpers mirroring the substrate contract.
static ToolResult prepared(const QString& draft = QStringLiteral("D1")) {
    return ToolResult::ok_data(QJsonObject{{"status", "prepared"}, {"draft_id", draft}});
}
static ToolResult prepare_rejected(const QString& reason = QStringLiteral("bounds")) {
    return ToolResult::ok_data(QJsonObject{{"status", "rejected"}, {"reason", reason}});
}
static ToolResult filled() {
    return ToolResult::ok_data(QJsonObject{{"status", "filled"}});
}
static ToolResult submit_rejected(const QString& reason) {
    return ToolResult::ok_data(QJsonObject{{"status", "rejected"}, {"reason", reason}});
}
// LIVE mock-broker shapes — mirror Part 1's additive OrderFlowTools fields
// (status stays "filled" == broker SUBMISSION success; broker_status carries
// the real execution truth).
static ToolResult live_filled(double price, double qty) {
    return ToolResult::ok_data(QJsonObject{{"status", "filled"}, {"broker_status", "filled"},
                                            {"fill_price", price}, {"filled_qty", qty},
                                            {"reconciled", true}});
}
static ToolResult live_open() {
    return ToolResult::ok_data(QJsonObject{{"status", "filled"}, {"broker_status", "open"}});
}
static ToolResult live_partial() {
    return ToolResult::ok_data(QJsonObject{{"status", "filled"}, {"broker_status", "partially_filled"}});
}

// ── Tests ──────────────────────────────────────────────────────────────────

class TstStrategyLoop : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8()); // empty DB ⇒ kill switch reads false.
    }

    // prepared → submit(paper) → filled; on_fill invoked.
    void prepared_then_submit_fills() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;
        // quantity/limit_price populated; unrelated evidence gates are disabled
        // below so this test isolates prepare/submit behavior.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.proposed, 1);
        QCOMPARE(sum.prepared, 1);
        QCOMPARE(sum.filled, 1);
        QCOMPARE(sum.rejected, 0);

        // submit_order was called with draft_id=="D1" and mode=="paper".
        bool found_submit = false;
        for (const auto& c : tc.calls) {
            if (c.first == "submit_order") {
                QCOMPARE(c.second.value("draft_id").toString(), QStringLiteral("D1"));
                QCOMPARE(c.second.value("mode").toString(), QStringLiteral("paper"));
                found_submit = true;
            }
        }
        QVERIFY2(found_submit, "expected a submit_order call");
        QCOMPARE(s.fills.size(), 1); // on_fill invoked once.
    }

    // Track 3 (AI arming path): a default-constructed RunConfig (submit_mode
    // left at its default) must still submit mode=="paper" — i.e. adding the
    // field did not change today's byte-identical paper behavior. Neither
    // cli.live_armed nor cli.trading_allowed is touched anywhere in this test.
    void default_submit_mode_is_paper() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        // cfg.submit_mode intentionally left untouched -- exercising the default.
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);
        bool found_submit = false;
        for (const auto& c : tc.calls) {
            if (c.first == "submit_order") {
                QCOMPARE(c.second.value("mode").toString(), QStringLiteral("paper"));
                found_submit = true;
            }
        }
        QVERIFY2(found_submit, "expected a submit_order call");
    }

    // Track 3 core wiring proof: RunConfig.submit_mode=="live" threads straight
    // through to the mocked submit_order call, with NO cli.live_armed/
    // cli.trading_allowed write anywhere and NO real broker in this test binary
    // (FakeToolCaller only). Proves the runner passes whatever mode the caller
    // set; submit_order itself remains the sole live-gate authority in prod.
    // Uses a live_filled() mock (broker_status=="filled") rather than the bare
    // paper filled() shape -- per #39-a, live mode trusts broker_status, not
    // status=="filled" alone (see the lifecycle_live_* tests below).
    void submit_mode_live_threads_to_submit_call() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", live_filled(10.0, 1.0));

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live"); // caller-set only; no arm flag touched.
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);
        bool found_submit = false;
        for (const auto& c : tc.calls) {
            if (c.first == "submit_order") {
                QCOMPARE(c.second.value("mode").toString(), QStringLiteral("live"));
                found_submit = true;
            }
        }
        QVERIFY2(found_submit, "expected a submit_order call carrying mode==live");
    }

    // A NON-halting submit rejection (e.g. a risk-floor reject) must NOT fire
    // on_fill — the order never executed, so a position-tracking strategy must
    // not book it. Gates the fix that scopes on_fill to the "filled" branch:
    // making on_fill unconditional makes this fail.
    void submit_rejected_does_not_fire_on_fill() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", submit_rejected("exceeds max order value"));

        FakeStrategy s;
        // quantity/limit_price so prepare/submit receives a complete intent.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.prepared, 1);
        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.rejected, 1);
        QVERIFY2(s.fills.isEmpty(), "on_fill must NOT fire on a rejected (non-filled) submit");
        QVERIFY2(!sum.halted_by_kill_switch, "a plain risk rejection must not halt the loop");
    }

    // prepare rejected → no submit; counted as rejected.
    void rejected_no_submit() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepare_rejected());

        FakeStrategy s;
        // Evidence gates are disabled below because prepare rejection is what
        // this test exercises.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(tc.count("submit_order"), 0);
        QCOMPARE(sum.rejected, 1);
        QCOMPARE(sum.prepared, 0);
        QCOMPARE(sum.filled, 0);
    }

    // submit returns a kill-switch reason → loop halts cleanly; no extra ticks.
    void halt_on_submit_kill_switch_reason() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", submit_rejected("kill switch engaged"));
        // Script enough for further ticks to prove they do NOT happen.
        for (int i = 0; i < 5; ++i) {
            tc.enqueue("prepare_order", prepared("DX"));
            tc.enqueue("submit_order", filled());
        }

        FakeStrategy s;
        // Evidence gates are disabled below so submit kill-switch handling is isolated.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 5, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.ticks, 1); // loop broke on the very first tick — no spin.
        QVERIFY(sum.halted_by_kill_switch);
        QCOMPARE(sum.filled, 0);
        QCOMPARE(tc.count("submit_order"), 1); // only the halting submit ran.
    }

    // No intents; max_iters bounds the loop; get_quote runs each tick.
    void max_iters_bounds_loop() {
        FakeToolCaller tc;
        FakeStrategy s;
        s.intents_ = {}; // propose nothing.

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 3, .require_floor = false});

        QCOMPARE(sum.ticks, 3);
        QCOMPARE(sum.proposed, 0);
        QCOMPARE(tc.count("get_quote"), 3); // universe has one symbol, one fetch/tick.
    }

    // ── Pre-trade guardrail integration (Task 2 of the pretrade-guardrail spec) ─

    // Gate runs BEFORE prepare_order: a below-cost intent is recorded in
    // gate_rejections and NEVER reaches prepare/submit; a clean intent passes
    // through the existing prepare→submit(paper) pipeline unchanged.
    void gate_rejects_below_cost_intent_and_records_it() {
        FakeStrategy s;
        // Two intents: BTC-USD (will be gate-rejected), ETH-USD (will pass).
        s.intents_ = {
            TradeIntent{{"symbol", "BTC-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 100.0}},
            TradeIntent{{"symbol", "ETH-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 50.0}},
        };
        FakeToolCaller tc; // prepare_order/submit_order default to success
        StrategyRunner runner;
        // Inject assess: BTC-USD -> below cost (clears_cost false); ETH-USD -> all good.
        runner.assess_fn = [](const QString& sym) {
            openmarketterminal::ai_decision::DecisionPacket p;
            p.has_edge_signal = true;
            p.clears_cost = sym == QStringLiteral("BTC-USD") ? QStringLiteral("false") : QStringLiteral("true");
            p.freshness = QStringLiteral("ok");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});

        // BTC-USD gate-rejected: recorded, and NEVER submitted.
        QCOMPARE(sum.gate_rejections.size(), 1);
        QCOMPARE(sum.gate_rejections[0].symbol, QStringLiteral("BTC-USD"));
        QCOMPARE(sum.gate_rejections[0].rule, QStringLiteral("cost"));
        // No prepare_order/submit_order call carried symbol BTC-USD; submit_order only ever mode:paper.
        for (const auto& c : tc.calls) {
            if (c.first == QLatin1String("prepare_order"))
                QVERIFY(c.second.value("symbol").toString() != QLatin1String("BTC-USD"));
            if (c.first == QLatin1String("submit_order"))
                QCOMPARE(c.second.value("mode").toString(), QStringLiteral("paper"));
        }
        // ETH-USD passed the gate -> a prepare_order happened for it.
        bool eth_prepared = false;
        for (const auto& c : tc.calls)
            if (c.first == QLatin1String("prepare_order") && c.second.value("symbol").toString() == QLatin1String("ETH-USD"))
                eth_prepared = true;
        QVERIFY(eth_prepared);
    }

    // ── Deterministic floor integration (Task 2 of the deterministic-floor spec) ─

    void floor_permits_endorsed_intent() {
        ensure_migrated_db();
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DF1"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","FLR-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("buy");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.filled, 1);
        QCOMPARE(sum.floor_skipped, 0);
    }

    void floor_skips_non_endorsed_intent() {
        ensure_migrated_db();
        FakeToolCaller tc;  // no prepare/submit should be reached
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","NFL-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "watch"; p.clears_cost = "true"; p.freshness = "ok";
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.floor_skipped, 1);
        QCOMPARE(sum.floor_skips.size(), 1);
        QCOMPARE(sum.floor_skips.at(0).rule, QStringLiteral("floor"));
        QCOMPARE(tc.count("submit_order"), 0);
        QCOMPARE(sum.gate_rejections.size(), 0);   // floor-skip is NOT a gate-reject
    }

    void floor_default_skips_default_packet() {
        ensure_migrated_db();
        FakeToolCaller tc;
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","DEF-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };  // has_edge_signal=false
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.floor_skipped, 1);            // default-ON floor skips a non-endorsed default packet
        QCOMPARE(sum.filled, 0);
    }

    void require_floor_off_lets_non_endorsed_through() {
        ensure_migrated_db();
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DOF"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","OFF-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);
        QCOMPARE(sum.floor_skipped, 0);            // floor disabled -> proceeds past the floor
        QCOMPARE(sum.filled, 1);
    }

    void floor_fail_to_skip_on_assess_throw() {
        ensure_migrated_db();
        FakeToolCaller tc;
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","THR-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) -> ai_decision::DecisionPacket { throw std::runtime_error("boom"); };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.errors, 1);                   // catch incremented errors
        QCOMPARE(sum.floor_skipped, 1);            // degraded packet -> floor skip
        QCOMPARE(sum.filled, 0);
    }

    // ── Floor exemption for de-risking intents (⑤d, Task 2) ─────────────────

    void floor_allows_unendorsed_reducing_exit() {
        ensure_migrated_db();
        // Seed a LONG position for fake/DRK-USD (buy 10) so a sell reduces it.
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('drk1','fake','DRK-USD','buy',10,100,0,0,1000,'d')").is_ok());
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DDRK"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","DRK-USD"},{"side","sell"},{"quantity",4.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {                 // NON-endorsing (gate=watch)
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "watch"; p.clears_cost = "true"; p.freshness = "ok";
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.floor_skipped, 0);                          // de-risking exit NOT floor-skipped
        QCOMPARE(sum.filled, 1);                                 // proceeded to submit(paper)
    }

    void floor_still_skips_unendorsed_opening_buy() {
        ensure_migrated_db();
        FakeToolCaller tc;
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","OPN-USD"},{"side","buy"},{"quantity",4.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {                 // NON-endorsing
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "watch"; p.clears_cost = "true"; p.freshness = "ok";
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.floor_skipped, 1);                          // opening from flat still blocked
        QCOMPARE(sum.filled, 0);
    }

    // ── Direction-agreement floor gate (F2) ──────────────────────────────────

    void floor_skips_direction_disagreement() {
        ensure_migrated_db();
        FakeToolCaller tc;  // no prepare/submit should be reached
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","DIR-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {                 // fully endorsed, but SHORT
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("short");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.floor_skipped, 1);
        QCOMPARE(sum.floor_skips.size(), 1);
        QCOMPARE(sum.floor_skips.at(0).rule, QStringLiteral("floor"));
        QVERIFY2(sum.floor_skips.at(0).reason.contains(QStringLiteral("opposite side")),
                  qUtf8Printable(sum.floor_skips.at(0).reason));
        QCOMPARE(sum.gate_rejections.size(), 0);
        QCOMPARE(tc.count("submit_order"), 0);
    }

    void floor_permits_direction_agreement() {
        ensure_migrated_db();
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DDIR"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","DIA-USD"},{"side","buy"},{"quantity",1.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("buy");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.filled, 1);
        QCOMPARE(sum.floor_skipped, 0);
    }

    // The edge here fully ENDORSES the long side (gate=pass/cost/fresh, side=
    // "buy") — that's the SAME direction that would make a plain "sell" fail
    // intent_agrees_with_edge (sell vs. buy is a directional disagreement, so
    // dir_ok is false and fv.ok is true: the floor's (!fv.ok || !dir_ok) term
    // evaluates true on its own). With an existing long position, this SELL
    // is de-risking (quantity < existing net), so ONLY intent_reduces_exposure
    // — the exemption under test — can be what keeps it from being floor-skipped.
    // (Using an edge that instead endorses "short" for a sell, as this test
    // previously did, makes intent_agrees_with_edge true by itself, so the
    // skip is bypassed before the exemption clause is ever reached — that
    // version passed even with the exemption deleted, i.e. it was tautological.)
    void floor_exempts_reduce_on_flipped_edge() {
        ensure_migrated_db();
        // Seed an EXISTING long position for fake/FLP2-USD (buy 10).
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('flp1','fake','FLP2-USD','buy',10,100,0,0,1000,'d')").is_ok());
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DFLP"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        // Reducing SELL: quantity < existing net.
        s.intents_ = {TradeIntent{{"symbol","FLP2-USD"},{"side","sell"},{"quantity",4.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {                 // still endorses LONG — disagrees with the sell
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("buy");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});
        QCOMPARE(sum.floor_skipped, 0);   // de-risking exit NOT floor-skipped, even against a disagreeing edge
        QCOMPARE(sum.filled, 1);
    }

    // ── MeanReversionStrategy (Task 2) ───────────────────────────────────────
    // Driven directly with hand-built snapshots — deterministic, no runner.

    // Build a single-quote snapshot for AAPL.
    static MarketSnapshot snap(double price) {
        MarketSnapshot s;
        s.quotes["AAPL"] = price;
        return s;
    }
    static TradeIntent buy_intent() {
        return TradeIntent{{"symbol", "AAPL"}, {"side", "buy"}};
    }

    // window=3: only 2 prices seen ⇒ still warming up, propose nothing.
    void meanrev_warmup_no_intent() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        QVERIFY(s.propose(snap(100)).isEmpty());
        QVERIFY(s.propose(snap(100)).isEmpty());
    }

    // Stable price at the mean ⇒ inside the band ⇒ no intent.
    void meanrev_in_band_no_intent() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        s.propose(snap(100));
        s.propose(snap(100));
        s.propose(snap(100));            // window full, mean 100.
        QVERIFY(s.propose(snap(100)).isEmpty()); // price == mean ⇒ in band.
    }

    // Dip below the band while flat ⇒ exactly one BUY at the dip price.
    void meanrev_dip_buys() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        s.propose(snap(100));
        s.propose(snap(100));
        s.propose(snap(100));
        const auto intents = s.propose(snap(98)); // window [100,100,98], mean 99.33; 98 < 98.34.
        QCOMPARE(intents.size(), 1);
        const TradeIntent& i = intents.first();
        QCOMPARE(i.value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(i.value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i.value("order_type").toString(), QStringLiteral("limit"));
        QVERIFY(qFuzzyCompare(i.value("quantity").toDouble(), 10.0));
        QVERIFY(qFuzzyCompare(i.value("limit_price").toDouble(), 98.0));
    }

    // Rip above the band but flat (not holding) ⇒ no SELL.
    void meanrev_rip_without_holding_no_sell() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        s.propose(snap(100));
        s.propose(snap(100));
        s.propose(snap(100));
        QVERIFY(s.propose(snap(102)).isEmpty()); // rip, but nothing to close.
    }

    // Rip above the band WHILE holding (filled buy recorded) ⇒ one SELL.
    void meanrev_rip_while_holding_sells() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        s.on_fill(buy_intent(), QJsonObject{{"status", "filled"}}); // now holding AAPL.
        s.propose(snap(100));
        s.propose(snap(100));
        s.propose(snap(100));
        const auto intents = s.propose(snap(102)); // window [100,100,102], mean 100.67; 102 > 101.67.
        QCOMPARE(intents.size(), 1);
        const TradeIntent& i = intents.first();
        QCOMPARE(i.value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(i.value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i.value("order_type").toString(), QStringLiteral("limit"));
        QVERIFY(qFuzzyCompare(i.value("limit_price").toDouble(), 102.0));
    }

    // on_fill with a non-"filled" status must NOT set holding ⇒ a later rip
    // proposes no SELL.
    void meanrev_on_fill_ignores_non_fill() {
        MeanReversionStrategy s({"AAPL"}, /*window=*/3);
        s.on_fill(buy_intent(), QJsonObject{{"status", "rejected"}}); // ignored.
        s.propose(snap(100));
        s.propose(snap(100));
        s.propose(snap(100));
        QVERIFY(s.propose(snap(102)).isEmpty()); // not holding ⇒ no close.
    }

    // ── LlmStrategy (Task 3, typed-verb surface: Task 2 of piece ⑤c) ────────
    // Fully fake-driven: a CompletionFn lambda returns canned text; the LLM
    // output is UNTRUSTED, so parse_actions only enforces structural sanity +
    // universe membership and must NEVER throw.

    static QStringList aapl_universe() { return {QStringLiteral("AAPL")}; }

    void llm_clean_array_one_action() {
        const QString reply = R"([{"symbol":"AAPL","action":"enter","conviction":0.5}])";
        const auto acts = LlmStrategy::parse_actions(reply, aapl_universe());
        QCOMPARE(acts.size(), 1);
        QCOMPARE(acts.first().symbol, QStringLiteral("AAPL"));
        QVERIFY(acts.first().action == ActionType::Enter);
        QVERIFY(qFuzzyCompare(acts.first().conviction, 0.5));
    }
    void llm_prose_wrapped_parses() {
        const QString reply = "Sure:\n```json\n"
            R"([ {"symbol":"AAPL","action":"exit"} ])" "\n```";
        const auto acts = LlmStrategy::parse_actions(reply, aapl_universe());
        QCOMPARE(acts.size(), 1);
        QVERIFY(acts.first().action == ActionType::Exit);
    }
    void llm_empty_array_zero() {
        QCOMPARE(LlmStrategy::parse_actions(QStringLiteral("[]"), aapl_universe()).size(), 0);
    }
    void llm_malformed_zero_no_throw() {
        QCOMPARE(LlmStrategy::parse_actions(QStringLiteral("not json at all"), aapl_universe()).size(), 0);
    }
    void llm_empty_completion_zero() {
        QCOMPARE(LlmStrategy::parse_actions(QString(), aapl_universe()).size(), 0);
    }
    void llm_out_of_universe_dropped() {
        const QString reply = R"([{"symbol":"TSLA","action":"enter","conviction":1.0}])";
        QCOMPARE(LlmStrategy::parse_actions(reply, aapl_universe()).size(), 0);
    }
    void llm_missing_fields_dropped() {
        QCOMPARE(LlmStrategy::parse_actions(R"([{"symbol":"AAPL"}])", aapl_universe()).size(), 0);      // no action
        QCOMPARE(LlmStrategy::parse_actions(R"([{"action":"enter"}])", aapl_universe()).size(), 0);     // no symbol
        QCOMPARE(LlmStrategy::parse_actions(R"([{"symbol":"AAPL","action":"bogus"}])", aapl_universe()).size(), 0);
    }
    void llm_conviction_absent_defaults_one() {
        const auto acts = LlmStrategy::parse_actions(R"([{"symbol":"AAPL","action":"enter"}])", aapl_universe());
        QCOMPARE(acts.size(), 1);
        QVERIFY(qFuzzyCompare(acts.first().conviction, 1.0));
    }
    void llm_propose_prompt_has_universe_and_translates() {
        QString captured;
        LlmStrategy::CompletionFn fake = [&](const QString& prompt) -> QString {
            captured = prompt;
            return R"([{"symbol":"AAPL","action":"enter","conviction":1.0}])";
        };
        LlmStrategy strat({QStringLiteral("AAPL"), QStringLiteral("MSFT")}, fake);  // max_qty default 10
        MarketSnapshot s;
        s.quotes["AAPL"] = 150.0;
        const auto intents = strat.propose(s);   // position_of degrades to flat w/o DB; enter is position-independent
        QCOMPARE(intents.size(), 1);
        QCOMPARE(intents.first().value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(intents.first().value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(intents.first().value("quantity").toDouble(), 10.0);            // conviction 1.0 * max 10
        QVERIFY2(captured.contains(QStringLiteral("MSFT")), "prompt must include the universe");
        QVERIFY2(captured.contains(QStringLiteral("enter")), "prompt must describe the typed verbs");

        LlmStrategy null_strat(aapl_universe(), nullptr);
        QVERIFY(null_strat.propose(s).isEmpty());
    }

    // ── AI paper ledger (Task 4) write-hook integration ─────────────────────
    // These two slots need a MIGRATED DB (the pre-existing slots above run
    // against an empty, unmigrated DB — that's what keeps their kill-switch
    // read at false). Declared last so the empty-DB slots run first/unaffected.

  private:
    void ensure_migrated_db() {
        static bool up = false;
        if (up) return;
        headless::HeadlessRuntime hr;
        headless::InitResult ir = hr.init(QString{});
        QVERIFY2(ir.ok, qUtf8Printable(ir.error));
        up = true;
    }

  private slots:
    // A filled paper intent must produce exactly one ai_fill row tagged with
    // the strategy's handler name, symbol, side/qty/price, and draft_id.
    void fill_writes_one_ai_fill_row() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLED"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;  // name() == "fake"
        s.intents_ = {TradeIntent{{"symbol", "LED-USD"}, {"side", "buy"}, {"quantity", 3.0}, {"limit_price", 40.0}}};

        StrategyRunner runner;
        // This test isolates fill persistence, so cost/freshness gates are
        // explicitly disabled rather than relying on missing telemetry.
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            return p;
        };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);
        QCOMPARE(sum.filled, 1);

        auto rows = AiFillRepository::instance().list("fake", "LED-USD", 10);
        QVERIFY(rows.is_ok());
        QCOMPARE(rows.value().size(), 1);
        QCOMPARE(rows.value().at(0).handler, QStringLiteral("fake"));
        QCOMPARE(rows.value().at(0).symbol, QStringLiteral("LED-USD"));
        QCOMPARE(rows.value().at(0).quantity, 3.0);
        QCOMPARE(rows.value().at(0).fill_price, 40.0);  // limit_price used
        QCOMPARE(rows.value().at(0).draft_id, QStringLiteral("DLED"));
    }

    // A filled paper intent must record the real 3-bps paper fee (F3), not
    // fee=0.0 — mirroring UnifiedTrading::init_session's paper fee_rate. The
    // fee folds into cost basis on this open, so realized_pnl stays 0 and
    // avg_entry_price is worsened by fee/qty.
    void fill_records_real_paper_fee() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DFEE"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;  // name() == "fake"
        s.intents_ = {TradeIntent{{"symbol", "FEE-USD"}, {"side", "buy"}, {"quantity", 3.0}, {"limit_price", 40.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            return p;
        };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        RunSummary sum = runner.run(s, tc, cfg);
        QCOMPARE(sum.filled, 1);

        auto rows = AiFillRepository::instance().list("fake", "FEE-USD", 10);
        QVERIFY(rows.is_ok());
        QCOMPARE(rows.value().size(), 1);
        const double expected_fee = 3.0 * 40.0 * 0.0003;  // qty * fill_price * kPaperFeeRate
        QVERIFY2(qAbs(rows.value().at(0).fee - expected_fee) < 1e-9,
                  qUtf8Printable(QString::number(rows.value().at(0).fee)));
        QCOMPARE(rows.value().at(0).realized_pnl, 0.0);  // open books no realized P&L (fee lives in basis)

        ai_ledger::LedgerPosition pos = ai_ledger::position_of("fake", "FEE-USD");
        const double expected_avg = 40.0 + expected_fee / 3.0;
        QVERIFY2(qAbs(pos.avg_entry_price - expected_avg) < 1e-9,
                  qUtf8Printable(QString::number(pos.avg_entry_price)));
        QVERIFY(pos.avg_entry_price > 40.0);  // fee-adjusted basis for a long is worse than fill_price
    }

    // A gate-rejected intent never reaches prepare/submit, so it must write
    // no ai_fill row at all.
    void gate_rejected_intent_writes_no_row() {
        ensure_migrated_db();

        FakeToolCaller tc;  // no prepare/submit needed — gate rejects first
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "REJ-USD"}, {"side", "buy"}, {"quantity", 3.0}, {"limit_price", 40.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.clears_cost = QStringLiteral("false");  // cost gate rejects
            return p;
        };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = true;
        RunSummary sum = runner.run(s, tc, cfg);
        QCOMPARE(sum.filled, 0);

        auto rows = AiFillRepository::instance().list("fake", "REJ-USD", 10);
        QVERIFY(rows.is_ok());
        QCOMPARE(rows.value().size(), 0);  // rejected before submit → no fill row
    }

    // ── Runner wiring: cumulative + handler-scoped cap via ledger (Task 3) ──

    void cumulative_cap_rejects_growing_intent_over_existing() {
        ensure_migrated_db();
        // Existing ledger position for fake/CAP-USD = +8 (buy 8).
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('cap1','fake','CAP-USD','buy',8,100,0,0,1000,'d')").is_ok());

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DCAP"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","CAP-USD"},{"side","buy"},{"quantity",5.0},{"limit_price",100.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.max_position_qty = 10.0;  // 8 + 5 = 13 > 10 and > 8 -> reject
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 0);
        QCOMPARE(tc.count("submit_order"), 0);              // never submitted
        QCOMPARE(sum.gate_rejections.size(), 1);
        QCOMPARE(sum.gate_rejections.at(0).rule, QStringLiteral("position"));
    }

    void cumulative_cap_allows_reducing_intent_over_cap() {
        ensure_migrated_db();
        // Existing fake/RED-USD = +15 (over cap 10).
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('red1','fake','RED-USD','buy',15,100,0,0,1000,'d')").is_ok());

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DRED"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","RED-USD"},{"side","sell"},{"quantity",3.0},{"limit_price",100.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.max_position_qty = 10.0;  // 15 - 3 = 12, still > cap 10 but |12| <= |15| (reduces) -> allowed
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);                            // reduce passed to submit
        QVERIFY(tc.count("submit_order") >= 1);
    }

    // ── Runner wiring: cross-handler aggregate cap injection (Task 2) ──────

    void aggregate_cap_rejects_growing_cross_handler() {
        ensure_migrated_db();
        // Another handler already holds +8 on AGG-USD; 'fake' holds nothing.
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('ag1','other','AGG-USD','buy',8,100,0,0,1000,'d')").is_ok());
        FakeToolCaller tc;
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","AGG-USD"},{"side","buy"},{"quantity",5.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("buy");
            return p;  // endorsed, so the floor lets it reach the gate
        };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1};
        cfg.max_aggregate_position_qty = 10.0;   // aggregate 8 + 5 = 13 > 10 -> reject
        RunSummary sum = runner.run(s, tc, cfg);
        QCOMPARE(sum.filled, 0);
        QCOMPARE(tc.count("submit_order"), 0);
        QVERIFY(sum.gate_rejections.size() >= 1);
        QCOMPARE(sum.gate_rejections.at(0).rule, QStringLiteral("aggregate"));
    }

    void aggregate_cap_off_lets_it_through() {
        ensure_migrated_db();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('ag2','other','AGO-USD','buy',8,100,0,0,1000,'d')").is_ok());
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DAGO"));
        tc.enqueue("submit_order", filled());
        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol","AGO-USD"},{"side","buy"},{"quantity",5.0},{"limit_price",100.0}}};
        StrategyRunner runner;
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            p.has_edge_signal = true; p.gate = "pass"; p.clears_cost = "true"; p.freshness = "ok";
            p.side = QStringLiteral("buy");
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});  // no agg cap
        QCOMPARE(sum.filled, 1);
    }

    // Track 3: the kill switch dominates submit_mode. Even with submit_mode==
    // "live" set on the config (never armed here -- cli.live_armed/
    // cli.trading_allowed are NEVER written by this test), a REAL engaged
    // cli.kill_switch halts the loop on tick 1 (StrategyRunner's top-of-tick
    // check), before get_quote/prepare_order/submit_order are ever called.
    void kill_switch_halts_before_any_submit_even_when_mode_live() {
        ensure_migrated_db();
        QVERIFY(SettingsRepository::instance().set("cli.kill_switch", "true", "cli").is_ok());

        FakeToolCaller tc;  // scripted but must NEVER be reached
        tc.enqueue("prepare_order", prepared("DKILL"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "KILL-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunConfig cfg{.interval_sec = 0, .max_iters = 5, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live");  // even an (unarmed) "live" mode must not matter.
        RunSummary sum = runner.run(s, tc, cfg);

        // Restore immediately so later slots (and re-runs) see a clean switch.
        QVERIFY(SettingsRepository::instance().set("cli.kill_switch", "false", "cli").is_ok());

        QVERIFY(sum.halted_by_kill_switch);
        QCOMPARE(sum.ticks, 1);  // halted at the very top of tick 1 -- no spin.
        QCOMPARE(tc.count("prepare_order"), 0);
        QCOMPARE(tc.count("submit_order"), 0);
    }

    // ── #39-a mock-broker lifecycle: honest live fill, no phantom ai_fill ──
    //
    // Five cases exercising the mode-aware fill block. All LIVE cases use
    // submit_order responses shaped like Part 1's additive OrderFlowTools
    // fields (status=="filled" is broker SUBMISSION success only;
    // broker_status carries the real execution truth). In every LIVE case
    // the ai_fill ledger must stay empty -- the live realized-P&L ledger is
    // recorded by submit_order itself (reconcile_and_record), never by the
    // runner.

    // (1) PAPER regression: unchanged behavior -- status=="filled" books a
    // real ai_fill row, byte-identical to today.
    void lifecycle_paper_filled_books_ai_fill_row() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLIFEP"));
        tc.enqueue("submit_order", filled());

        FakeStrategy s;  // name() == "fake"
        s.intents_ = {TradeIntent{{"symbol", "LIFEP-USD"}, {"side", "buy"}, {"quantity", 2.0}, {"limit_price", 50.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        // cfg.submit_mode left at default ("paper").
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);
        QCOMPARE(sum.live_submitted, 0);
        auto rows = AiFillRepository::instance().list("fake", "LIFEP-USD", 10);
        QVERIFY(rows.is_ok());
        QCOMPARE(rows.value().size(), 1);  // paper still books.
    }

    // (2) LIVE filled: broker_status=="filled" is a real execution -- counted
    // as filled, on_fill fires, but NO ai_fill row is ever created (the live
    // ledger was already recorded by submit_order via reconcile_and_record).
    void lifecycle_live_filled_counts_no_phantom_ai_fill() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLIFEL"));
        tc.enqueue("submit_order", live_filled(100.0, 5.0));

        FakeStrategy s;  // name() == "fake"
        s.intents_ = {TradeIntent{{"symbol", "LIFEL-USD"}, {"side", "buy"}, {"quantity", 5.0}, {"limit_price", 100.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live");
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);
        QCOMPARE(sum.rejected, 0);
        QCOMPARE(sum.live_submitted, 0);
        QCOMPARE(s.fills.size(), 1);  // on_fill DID fire -- it's a real execution.

        auto rows = AiFillRepository::instance().list("fake", "LIFEL-USD", 10);
        QVERIFY(rows.is_ok());
        QVERIFY2(rows.value().isEmpty(),
                  "live-filled must NOT book a phantom paper ai_fill row -- the live "
                  "ledger is submit_order's job");
    }

    // (3) LIVE open: broker accepted the order but has not executed it yet.
    // Submission-success ("status":"filled") must NOT be mistaken for an
    // execution -- not filled, not rejected, counted separately, no booking.
    void lifecycle_live_open_not_filled_not_booked() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLIFEO"));
        tc.enqueue("submit_order", live_open());

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "LIFEO-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live");
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.rejected, 0);
        QCOMPARE(sum.live_submitted, 1);
        QVERIFY(s.fills.isEmpty());  // on_fill must not fire on a non-execution.

        auto rows = AiFillRepository::instance().list("fake", "LIFEO-USD", 10);
        QVERIFY(rows.is_ok());
        QVERIFY(rows.value().isEmpty());
    }

    // (4) LIVE partially_filled: same treatment as open for booking purposes
    // (a known simplification -- partial fills are not split-booked here).
    void lifecycle_live_partial_not_filled_not_booked() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLIFEPA"));
        tc.enqueue("submit_order", live_partial());

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "LIFEPA-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live");
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.rejected, 0);
        QCOMPARE(sum.live_submitted, 1);
        QVERIFY(s.fills.isEmpty());

        auto rows = AiFillRepository::instance().list("fake", "LIFEPA-USD", 10);
        QVERIFY(rows.is_ok());
        QVERIFY(rows.value().isEmpty());
    }

    // (5) LIVE rejected: unchanged -- a real broker rejection is still
    // counted as rejected, no fill, no booking.
    void lifecycle_live_rejected_counts_rejected() {
        ensure_migrated_db();

        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("DLIFER"));
        tc.enqueue("submit_order", submit_rejected("broker denied"));

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "LIFER-USD"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
        cfg.require_cost_gate = false;
        cfg.require_freshness_gate = false;
        cfg.submit_mode = QStringLiteral("live");
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 0);
        QCOMPARE(sum.rejected, 1);
        QCOMPARE(sum.live_submitted, 0);
        QVERIFY(s.fills.isEmpty());

        auto rows = AiFillRepository::instance().list("fake", "LIFER-USD", 10);
        QVERIFY(rows.is_ok());
        QVERIFY(rows.value().isEmpty());
    }

    // ── #39 (P-E) live position cap sources the LIVE ledger, not ai_fill ────
    //
    // Seeds a LIVE position via LivePnlRepository::insert_open under the SAME
    // account the runner reads (mcp::cli_allowed_account()) and venue "equity"
    // (the loop always drives equity). cli.allowed_account is explicitly set
    // here to a real account string rather than left unset: a default-unset
    // cli_allowed_account() returns a NULL QString (not merely empty), and
    // Qt's SQLite driver binds a null QString as SQL NULL -- `account = NULL`
    // never matches any row (not even one stored with account=''), and
    // inserting a NULL into the NOT NULL `account` column fails outright. A
    // real live account is always configured before live trading is armed in
    // practice, so this also matches production more closely than an unset
    // account would. Proves three things in one seed:
    //   (a) a LIVE run whose growing BUY would push the live position past the
    //       cap is GATE-REJECTED with rule "position" -- the cap saw the REAL
    //       live position, not the (empty) paper ai_fill ledger.
    //   (b) a LIVE reducing SELL against the same live position passes
    //       (increase-only rule) -- the source isn't just "always reject".
    //   (c) a PAPER run against the identical symbol, with NO ai_fill seeded,
    //       sees FLAT (not the live-seeded 8) and the growing buy PASSES --
    //       proving the position source actually switches by mode rather than
    //       always reading the live ledger.
    void live_position_cap_reads_live_ledger_and_paper_stays_on_ai_fill() {
        ensure_migrated_db();

        QVERIFY(SettingsRepository::instance().set("cli.allowed_account", "acct-lpos", "cli").is_ok());
        QCOMPARE(mcp::cli_allowed_account(), QStringLiteral("acct-lpos"));

        LivePosition seed;
        seed.account = mcp::cli_allowed_account();  // matches what the runner's live branch reads
        seed.venue = QStringLiteral("equity");
        seed.instrument = QStringLiteral("LPOS-USD");
        seed.qty = 8.0;
        seed.avg_cost = 100.0;
        seed.cost_basis = 800.0;
        seed.opened_at = QStringLiteral("2026-01-01T00:00:00Z");
        seed.status = QStringLiteral("open");
        auto ins_r = LivePnlRepository::instance().insert_open(seed);
        QVERIFY2(ins_r.is_ok(), qUtf8Printable(QString::fromStdString(ins_r.is_err() ? ins_r.error() : "")));

        // (a) growing buy: 8 + 5 = 13 > cap 10 and > existing 8 -> gate-rejected "position".
        {
            FakeToolCaller tc;  // no prepare/submit should be reached
            FakeStrategy s;
            s.intents_ = {TradeIntent{{"symbol","LPOS-USD"},{"side","buy"},{"quantity",5.0},{"limit_price",100.0}}};
            StrategyRunner runner;
            runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
            RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
            cfg.require_cost_gate = false;
            cfg.require_freshness_gate = false;
            cfg.submit_mode = QStringLiteral("live");
            cfg.max_position_qty = 10.0;
            RunSummary sum = runner.run(s, tc, cfg);

            QCOMPARE(sum.filled, 0);
            QCOMPARE(tc.count("submit_order"), 0);
            QCOMPARE(sum.gate_rejections.size(), 1);
            QCOMPARE(sum.gate_rejections.at(0).rule, QStringLiteral("position"));
        }

        // (b) reducing sell: 8 - 5 = 3, |3| <= |8| -> passes the cap.
        {
            FakeToolCaller tc;
            tc.enqueue("prepare_order", prepared("DLPOSR"));
            tc.enqueue("submit_order", live_filled(100.0, 5.0));
            FakeStrategy s;
            s.intents_ = {TradeIntent{{"symbol","LPOS-USD"},{"side","sell"},{"quantity",5.0},{"limit_price",100.0}}};
            StrategyRunner runner;
            runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
            RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
            cfg.require_cost_gate = false;
            cfg.require_freshness_gate = false;
            cfg.submit_mode = QStringLiteral("live");
            cfg.max_position_qty = 10.0;
            RunSummary sum = runner.run(s, tc, cfg);

            QCOMPARE(sum.gate_rejections.size(), 0);
            QCOMPARE(sum.filled, 1);
        }

        // (c) PAPER, same symbol, no ai_fill seeded -> flat -> growing buy passes.
        {
            FakeToolCaller tc;
            tc.enqueue("prepare_order", prepared("DLPOSP"));
            tc.enqueue("submit_order", filled());
            FakeStrategy s;
            s.intents_ = {TradeIntent{{"symbol","LPOS-USD"},{"side","buy"},{"quantity",5.0},{"limit_price",100.0}}};
            StrategyRunner runner;
            runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };
            RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
            cfg.require_cost_gate = false;
            cfg.require_freshness_gate = false;
            // cfg.submit_mode left default ("paper").
            cfg.max_position_qty = 10.0;
            RunSummary sum = runner.run(s, tc, cfg);

            QCOMPARE(sum.gate_rejections.size(), 0);
            QCOMPARE(sum.filled, 1);
        }

        // Reset immediately so later slots (and re-runs) see a clean default.
        QVERIFY(SettingsRepository::instance().set("cli.allowed_account", "", "cli").is_ok());
    }
};

QTEST_MAIN(TstStrategyLoop)
#include "tst_strategy_loop.moc"
