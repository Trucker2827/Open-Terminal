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
#include "services/ai_decision/DecisionContext.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/Strategy.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "storage/repositories/AiFillRepository.h"
#include "storage/sqlite/Database.h"

using namespace openmarketterminal;
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
        // quantity/limit_price populated so the pre-trade guardrail (a no-op
        // here: default caps are 0 and no assess_fn is injected) allows the
        // intent through to prepare/submit unchanged.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});

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

    // A NON-halting submit rejection (e.g. a risk-floor reject) must NOT fire
    // on_fill — the order never executed, so a position-tracking strategy must
    // not book it. Gates the fix that scopes on_fill to the "filled" branch:
    // making on_fill unconditional makes this fail.
    void submit_rejected_does_not_fire_on_fill() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepared("D1"));
        tc.enqueue("submit_order", submit_rejected("exceeds max order value"));

        FakeStrategy s;
        // quantity/limit_price so the gate (no-op under default config) lets it through.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});

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
        // quantity/limit_price so the gate (no-op under default config) lets it through
        // to prepare_order, which is what this test exercises.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});

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
        // quantity/limit_price so the gate (no-op under default config) lets it through.
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"quantity", 1.0}, {"limit_price", 10.0}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 5, .require_floor = false});

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
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});
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

    // ── LlmStrategy (Task 3) ─────────────────────────────────────────────────
    // Fully fake-driven: a CompletionFn lambda returns canned text; the LLM
    // output is UNTRUSTED, so parse_intents only enforces structural sanity +
    // universe membership and must NEVER throw.

    static QStringList aapl_universe() { return {QStringLiteral("AAPL")}; }

    // Clean array, symbol in universe ⇒ exactly one intent with those fields.
    void llm_clean_array_one_intent() {
        const QString reply =
            R"([{"symbol":"AAPL","side":"buy","quantity":5,"order_type":"limit","limit_price":200}])";
        const auto intents = LlmStrategy::parse_intents(reply, aapl_universe());
        QCOMPARE(intents.size(), 1);
        const TradeIntent& i = intents.first();
        QCOMPARE(i.value("symbol").toString(), QStringLiteral("AAPL"));
        QCOMPARE(i.value("side").toString(), QStringLiteral("buy"));
        QVERIFY(qFuzzyCompare(i.value("quantity").toDouble(), 5.0));
        QCOMPARE(i.value("order_type").toString(), QStringLiteral("limit"));
        QVERIFY(qFuzzyCompare(i.value("limit_price").toDouble(), 200.0));
    }

    // Prose + markdown fence around the JSON ⇒ still parses one intent.
    void llm_prose_wrapped_parses() {
        const QString reply =
            "Sure, here you go:\n```json\n"
            R"([ {"symbol":"AAPL","side":"sell","quantity":1,"order_type":"limit","limit_price":210} ])"
            "\n```";
        const auto intents = LlmStrategy::parse_intents(reply, aapl_universe());
        QCOMPARE(intents.size(), 1);
        QCOMPARE(intents.first().value("side").toString(), QStringLiteral("sell"));
    }

    // Empty array ⇒ zero intents.
    void llm_empty_array_zero() {
        QCOMPARE(LlmStrategy::parse_intents(QStringLiteral("[]"), aapl_universe()).size(), 0);
    }

    // Pure prose / not JSON ⇒ zero intents, no throw.
    void llm_malformed_zero_no_throw() {
        QCOMPARE(LlmStrategy::parse_intents(QStringLiteral("not json at all"), aapl_universe()).size(),
                 0);
    }

    // Empty completion ⇒ zero intents (no '[' delimiter).
    void llm_empty_completion_zero() {
        QCOMPARE(LlmStrategy::parse_intents(QString(), aapl_universe()).size(), 0);
    }

    // Symbol outside the allowed universe ⇒ dropped.
    void llm_out_of_universe_dropped() {
        const QString reply = R"([{"symbol":"TSLA","side":"buy","quantity":1}])";
        QCOMPARE(LlmStrategy::parse_intents(reply, aapl_universe()).size(), 0);
    }

    // Missing side/quantity, and missing symbol ⇒ both dropped.
    void llm_missing_fields_dropped() {
        QCOMPARE(LlmStrategy::parse_intents(R"([{"symbol":"AAPL"}])", aapl_universe()).size(), 0);
        QCOMPARE(LlmStrategy::parse_intents(R"([{"side":"buy","quantity":1}])", aapl_universe()).size(),
                 0);
    }

    // propose() drives the seam: prompt carries the universe, and a null
    // CompletionFn yields a clean empty result (no throw, no crash).
    void llm_propose_prompt_has_universe_and_parses() {
        QString captured;
        LlmStrategy::CompletionFn fake = [&](const QString& prompt) -> QString {
            captured = prompt;
            return R"([{"symbol":"AAPL","side":"buy","quantity":2,"order_type":"limit","limit_price":150}])";
        };
        // Universe carries MSFT, which is NOT a quote symbol — so asserting the
        // prompt contains "MSFT" gates universe interpolation specifically (it
        // cannot leak in via the quotes JSON).
        LlmStrategy strat({QStringLiteral("AAPL"), QStringLiteral("MSFT")}, fake);

        MarketSnapshot s;
        s.quotes["AAPL"] = 150.0;
        const auto intents = strat.propose(s);

        QCOMPARE(intents.size(), 1);
        QCOMPARE(intents.first().value("symbol").toString(), QStringLiteral("AAPL"));
        QVERIFY2(captured.contains(QStringLiteral("MSFT")),
                 "prompt must include the allowed universe symbols");

        // Null completion function ⇒ clean empty, never invoked.
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
        // Make the gate pass for LED-USD (default-constructed packet ⇒
        // clears_cost/freshness are empty strings ⇒ neither gate rejects).
        runner.assess_fn = [](const QString&) {
            ai_decision::DecisionPacket p;
            return p;
        };
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1, .require_floor = false});
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
        runner.assess_fn = [](const QString&) { return ai_decision::DecisionPacket{}; };  // passes cost/freshness
        RunConfig cfg{.interval_sec = 0, .max_iters = 1, .require_floor = false};
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
        cfg.max_position_qty = 10.0;  // 15 - 3 = 12, still > cap 10 but |12| <= |15| (reduces) -> allowed
        RunSummary sum = runner.run(s, tc, cfg);

        QCOMPARE(sum.filled, 1);                            // reduce passed to submit
        QVERIFY(tc.count("submit_order") >= 1);
    }
};

QTEST_MAIN(TstStrategyLoop)
#include "tst_strategy_loop.moc"
