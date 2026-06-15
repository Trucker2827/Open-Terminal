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

#include "mcp/McpTypes.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/Strategy.h"
#include "services/ai_strategy/StrategyRunner.h"

using namespace openmarketterminal;
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
        s.intents_ = {TradeIntent{{"symbol", "AAA"}, {"side", "buy"}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});

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

    // prepare rejected → no submit; counted as rejected.
    void rejected_no_submit() {
        FakeToolCaller tc;
        tc.enqueue("prepare_order", prepare_rejected());

        FakeStrategy s;
        s.intents_ = {TradeIntent{{"symbol", "AAA"}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 1});

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
        s.intents_ = {TradeIntent{{"symbol", "AAA"}}};

        StrategyRunner runner;
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 5});

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
        RunSummary sum = runner.run(s, tc, RunConfig{.interval_sec = 0, .max_iters = 3});

        QCOMPARE(sum.ticks, 3);
        QCOMPARE(sum.proposed, 0);
        QCOMPARE(tc.count("get_quote"), 3); // universe has one symbol, one fetch/tick.
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
};

QTEST_MAIN(TstStrategyLoop)
#include "tst_strategy_loop.moc"
