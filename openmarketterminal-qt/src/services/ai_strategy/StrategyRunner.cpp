// StrategyRunner.cpp — see StrategyRunner.h.
#include "services/ai_strategy/StrategyRunner.h"

#include "mcp/tools/LivePnl.h"
#include "mcp/tools/SettingsGate.h"
#include "services/ai_strategy/DeterministicFloor.h"
#include "services/ai_strategy/PretradeGate.h"
#include "services/ai_ledger/AiLedger.h"
#include "storage/repositories/LivePnlRepository.h"

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

// The paper rail (UnifiedTrading::init_session) charges this fee per fill; it is
// not surfaced by submit_order, so mirror it here to keep the ai_fill ledger's
// realized P&L truthful (a fee=0 record inflates the scorecard). Keep in sync
// with UnifiedTrading::init_session's paper-portfolio fee_rate.
constexpr double kPaperFeeRate = 0.0003;  // 3 bps

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
    policy.max_aggregate_position_qty = cfg.max_aggregate_position_qty;
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
                ++summary.errors;
                // assess_fn must never crash the loop. An error degrades to an
                // "unknown" packet, which fail-closed gates reject unless the
                // operator explicitly disabled those gates.
                pkt = ai_decision::DecisionPacket{};
                pkt.clears_cost = QStringLiteral("unknown");
                pkt.freshness = QStringLiteral("unknown");
            }
            gin.clears_cost = pkt.clears_cost;
            gin.freshness = pkt.freshness;
            gin.has_edge_signal = pkt.has_edge_signal;
            if (cfg.submit_mode == QLatin1String("live")) {
                // Live: the position cap must contain the REAL live position, not the
                // paper ai_fill ledger (empty in a live run). Source it from the live
                // realized-P&L ledger (read-only). The cross-handler AGGREGATE cap has no
                // live analog and is rejected for --mode live at the CLI, so leave it 0.
                const QString acct = mcp::cli_allowed_account();
                const QString venue = QStringLiteral("equity");  // the loop drives equity
                auto lp = LivePnlRepository::instance().get_open(acct, venue, sym);
                gin.existing_net_qty = (lp.is_ok() && lp.value().has_value())
                    ? lp.value().value().qty : 0.0;
                gin.aggregate_net_qty = 0.0;
            } else {
                gin.existing_net_qty = ai_ledger::position_of(s.name(), sym).net_qty;
                gin.aggregate_net_qty = ai_ledger::net_position_for_symbol(sym);
            }

            // Deterministic floor: runs BEFORE the pre-trade guardrail. Reads
            // only the already-resolved packet and, when it doesn't
            // positively endorse the symbol AND the intent is not de-risking
            // (⑤d: intent_reduces_exposure — reduce/close is always allowed so
            // the AI can exit an unendorsed position), records a floor_skip
            // (rule "floor", NEVER a gate_rejection) and skips — never reaches
            // evaluate_pretrade/prepare_order/submit_order. Default ON,
            // subtractive: it only prevents paper trades, never enables one.
            const FloorInputs fin{pkt.has_edge_signal, pkt.gate, pkt.clears_cost, pkt.freshness};
            const GateVerdict fv = floor_verdict(fin, FloorPolicy{cfg.require_floor});
            const QString intent_side = intent.value(QStringLiteral("side")).toString();
            // Direction agreement (F2): even a fully-endorsed edge (gate=pass/cost/
            // fresh) must recommend the SAME side the intent takes. A perp/chronos2
            // row can carry gate=="pass" with side=="short"/"sell" (a profitable
            // SHORT); a long-only enter must not ride a short endorsement. Enforced
            // only when the floor is on; de-risking is exempt (reduce/close below).
            const bool dir_ok =
                !cfg.require_floor || intent_agrees_with_edge(intent_side, pkt.side);
            if ((!fv.ok || !dir_ok) && !intent_reduces_exposure(intent, gin.existing_net_qty)) {
                const QString reason = !fv.ok ? fv.reason
                    : QStringLiteral("edge recommends opposite side");
                const QString rule = !fv.ok ? fv.rule : QStringLiteral("floor");
                ++summary.floor_skipped;
                summary.floor_skips.push_back({sym, intent_side, reason, rule});
                log(QStringLiteral("floor skipped %1: %2").arg(sym, reason));
                continue;  // before the pretrade gate / prepare_order / submit_order
            }

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
                               {{"draft_id", draft_id}, {"mode", cfg.submit_mode}});
            const QJsonObject subo = sub.data.toObject();
            const QString sub_reason = subo.value(QStringLiteral("reason")).toString();

            if (reason_is_halting(sub_reason)) {
                ++summary.rejected;
                log(QStringLiteral("submit halted: ") + sub_reason);
                summary.halted_by_kill_switch = true;
                halt = true;
                break;
            }

            const QString sub_status = subo.value(QStringLiteral("status")).toString();
            const bool is_live = (cfg.submit_mode == QLatin1String("live"));
            // LIVE: submit_order's status is "filled" on broker SUBMISSION success, not an
            // execution. Trust the reconciled broker_status; the live realized-P&L ledger is
            // recorded by submit_order (reconcile_and_record), so NEVER book a paper ai_fill.
            // PAPER: status=="filled" is a real paper fill; the ai_fill ledger is authoritative.
            const QString broker_status = subo.value(QStringLiteral("broker_status")).toString();
            const bool really_filled = is_live ? (broker_status == mcp::tools::kLiveFilledStatus)
                                               : (sub_status == QLatin1String("filled"));
            if (really_filled) {
                ++summary.filled;
                log(QStringLiteral("filled draft ") + draft_id);
                // on_fill notifies the strategy of ACTUAL fills only — a rejected
                // submit never happened, so a strategy tracking positions here must
                // not book it.
                s.on_fill(intent, subo);
                if (!is_live) {
                    // ── existing PAPER ai_fill record (UNCHANGED) ──
                    // Best-effort paper-ledger record — never break the loop on a DB error.
                    const double fill_price = intent.contains(QStringLiteral("limit_price"))
                        ? intent.value(QStringLiteral("limit_price")).toDouble()
                        : snap.quotes.value(sym, 0.0);
                    const double fill_qty = intent.value(QStringLiteral("quantity")).toDouble();
                    // Mirror the paper rail's fee so realized P&L is net of costs (F3).
                    const double fee = fill_qty * fill_price * kPaperFeeRate;
                    auto rec = ai_ledger::record_fill(
                        s.name(), sym, intent.value(QStringLiteral("side")).toString(),
                        fill_qty, fill_price, fee, draft_id);
                    if (rec.is_err())
                        log(QStringLiteral("ledger record skipped: ") + QString::fromStdString(rec.error()));
                }
            } else if (is_live && sub_status == QLatin1String("filled")) {
                // Live: submitted/accepted but not yet filled (broker_status open/partial/accepted/empty).
                // Not an execution — do NOT book, do NOT count as filled or rejected. Surface honestly.
                ++summary.live_submitted;
                log(QStringLiteral("live submitted draft %1 (broker_status=%2)")
                        .arg(draft_id, broker_status.isEmpty() ? QStringLiteral("unknown") : broker_status));
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
