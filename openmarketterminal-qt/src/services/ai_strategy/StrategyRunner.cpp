// StrategyRunner.cpp — see StrategyRunner.h.
#include "services/ai_strategy/StrategyRunner.h"

#include "mcp/tools/SettingsGate.h"
#include "services/ai_strategy/PretradeGate.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QThread>

#include <cstdio>

namespace openmarketterminal::ai_strategy {

namespace {

// A reason string signalling the substrate refused to trade at all — the loop
// must stop cleanly rather than spin re-trying every tick.
bool reason_is_halting(const QString& reason) {
    const QString r = reason.toLower();
    return r.contains(QLatin1String("kill switch engaged")) ||
           r.contains(QLatin1String("paper trading disabled"));
}

} // namespace

RunSummary StrategyRunner::run(Strategy& s, ToolCaller& tc, const RunConfig& cfg,
                               std::function<bool()> stop_requested) {
    RunSummary summary;

    // Pre-trade guardrail policy, derived once from cfg. Pure/subtractive:
    // evaluate_pretrade() only ever rejects (or is a no-op when caps are 0
    // and clears_cost/freshness come back "unknown") — it never opens a live
    // path or touches submit_order's mode:paper.
    GatePolicy policy;
    policy.max_notional_per_order = cfg.max_notional_per_order;
    policy.max_position_qty = cfg.max_position_qty;
    policy.allowed_venues = cfg.allowed_venues;
    policy.require_cost_gate = cfg.require_cost_gate;
    policy.require_freshness_gate = cfg.require_freshness_gate;

    auto log = [this](const QString& msg) {
        if (on_log) {
            on_log(msg);
        } else {
            std::fprintf(stdout, "[strategy] %s\n", msg.toUtf8().constData());
            std::fflush(stdout);
        }
    };

    QElapsedTimer clock;
    clock.start();
    bool halt = false;

    while (true) {
        ++summary.ticks;

        // 1) Kill switch is checked LIVE every tick — overrides everything.
        if (mcp::cli_kill_switch_engaged()) {
            log(QStringLiteral("halted by kill switch"));
            summary.halted_by_kill_switch = true;
            break;
        }

        // 2) Build the snapshot from reads only (best-effort, tolerant).
        MarketSnapshot snap;
        snap.ts = QDateTime::currentMSecsSinceEpoch();
        for (const QString& sym : s.universe()) {
            auto q = tc.call(QStringLiteral("get_quote"), {{"symbol", sym}});
            if (!q.success)
                continue;
            const double price = q.data.toObject().value(QStringLiteral("price")).toDouble();
            if (price != 0.0)
                snap.quotes.insert(sym, price);
        }
        auto p = tc.call(QStringLiteral("pm_paper_portfolio"), {});
        if (p.success)
            snap.portfolio = p.data.toObject();

        // 3) Ask the strategy for intents and run each through the pipeline.
        const QVector<TradeIntent> intents = s.propose(snap);
        summary.proposed += intents.size();

        for (const TradeIntent& intent : intents) {
            // Pre-trade guardrail: runs BEFORE prepare_order. Reject -> record
            // + continue (never submits). Allow -> falls through to the
            // existing, unchanged prepare_order -> submit_order(mode:paper)
            // path below.
            const QString sym = intent.value(QStringLiteral("symbol")).toString();
            GateInputs gin;
            gin.resolved_price = intent.contains(QStringLiteral("limit_price"))
                ? intent.value(QStringLiteral("limit_price")).toDouble()
                : snap.quotes.value(sym, 0.0);
            ai_decision::DecisionPacket pkt;
            try {
                pkt = assess_fn ? assess_fn(sym) : ai_decision::DecisionPacket{};
            } catch (...) {
                // assess_fn must never crash the loop; an error degrades to an
                // "unknown" packet, same as assess()'s own no-row/query-failed
                // path, so the cost/freshness gates fall through rather than
                // spuriously reject.
                pkt = ai_decision::DecisionPacket{};
                pkt.clears_cost = QStringLiteral("unknown");
                pkt.freshness = QStringLiteral("unknown");
            }
            gin.clears_cost = pkt.clears_cost;
            gin.freshness = pkt.freshness;
            gin.has_edge_signal = pkt.has_edge_signal;
            const GateVerdict gv = evaluate_pretrade(intent, gin, policy);
            if (!gv.ok) {
                ++summary.rejected;
                summary.gate_rejections.push_back(
                    {sym, intent.value(QStringLiteral("side")).toString(), gv.reason, gv.rule});
                log(QStringLiteral("gate rejected %1: %2").arg(sym, gv.reason));
                continue;
            }

            auto prep = tc.call(QStringLiteral("prepare_order"), intent);
            const QJsonObject prepo = prep.data.toObject();
            const QString prep_status = prepo.value(QStringLiteral("status")).toString();
            const QString prep_reason = prepo.value(QStringLiteral("reason")).toString();

            if (!prep.success || prep_status != QLatin1String("prepared")) {
                ++summary.rejected;
                log(QStringLiteral("prepare rejected: ") +
                    (prep_reason.isEmpty() ? prep.error : prep_reason));
                if (reason_is_halting(prep_reason) || reason_is_halting(prep.error)) {
                    summary.halted_by_kill_switch = true;
                    halt = true;
                    break;
                }
                continue;
            }

            ++summary.prepared;
            const QString draft_id = prepo.value(QStringLiteral("draft_id")).toString();

            auto sub = tc.call(QStringLiteral("submit_order"),
                               {{"draft_id", draft_id}, {"mode", QStringLiteral("paper")}});
            const QJsonObject subo = sub.data.toObject();
            const QString sub_reason = subo.value(QStringLiteral("reason")).toString();

            if (reason_is_halting(sub_reason)) {
                ++summary.rejected;
                log(QStringLiteral("submit halted: ") + sub_reason);
                summary.halted_by_kill_switch = true;
                halt = true;
                break;
            }

            if (subo.value(QStringLiteral("status")).toString() == QLatin1String("filled")) {
                ++summary.filled;
                log(QStringLiteral("filled draft ") + draft_id);
                // on_fill notifies the strategy of ACTUAL fills only — a rejected
                // submit never happened, so a strategy tracking positions here must
                // not book it.
                s.on_fill(intent, subo);
            } else {
                ++summary.rejected;
                log(QStringLiteral("submit rejected draft ") + draft_id + QStringLiteral(": ") +
                    sub_reason);
            }
        }

        if (halt)
            break;

        // 4) Stop conditions.
        if (cfg.max_iters > 0 && summary.ticks >= cfg.max_iters)
            break;
        if (cfg.duration_sec > 0 && clock.elapsed() >= static_cast<qint64>(cfg.duration_sec) * 1000)
            break;
        if (stop_requested && stop_requested())
            break;

        if (cfg.interval_sec > 0)
            QThread::sleep(static_cast<unsigned long>(cfg.interval_sec));
    }

    return summary;
}

} // namespace openmarketterminal::ai_strategy
