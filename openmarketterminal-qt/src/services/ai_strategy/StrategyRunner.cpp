// StrategyRunner.cpp — see StrategyRunner.h.
#include "services/ai_strategy/StrategyRunner.h"

#include "mcp/tools/SettingsGate.h"

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
