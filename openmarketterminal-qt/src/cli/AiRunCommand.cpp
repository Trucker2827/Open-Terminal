// AiRunCommand.cpp — see AiRunCommand.h.
#include "cli/AiRunCommand.h"

#include "cli/BridgeClient.h"
#include "cli/BridgeDiscovery.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpTypes.h"
#include "services/ai_decision/DecisionContext.h"
#include "services/ai_decision/Screener.h"
#include "services/ai_ledger/AiLedger.h"
#include "services/ai_ledger/Scorecard.h"
#include "services/ai_strategy/DeterministicFloor.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/AiFillRepository.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/sqlite/Database.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <optional>

namespace openmarketterminal::cli {

namespace {

// SIGINT → request a clean stop after the current tick (async-signal-safe: just
// flips an atomic the loop polls via stop_requested).
std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true); }

// Translate a bridge ClientResult into the in-process ToolResult shape the
// runner consumes. The daemon's POST /tool body IS a ToolResult::to_json()
// ({success,data,message,error}); a transport/HTTP failure maps to a failed
// ToolResult so the loop treats it as a tolerated read/prepare miss (it never
// crashes on a bad call).
mcp::ToolResult client_result_to_tool(const ClientResult& r) {
    if (r.status != ClientStatus::Ok)
        return mcp::ToolResult::fail(r.error.isEmpty() ? QStringLiteral("transport error") : r.error);
    mcp::ToolResult tr;
    const QJsonObject& b = r.body;
    tr.success = b.value(QStringLiteral("success")).toBool(false);
    tr.data = b.value(QStringLiteral("data"));
    tr.message = b.value(QStringLiteral("message")).toString();
    tr.error = b.value(QStringLiteral("error")).toString();
    return tr;
}

// Real ToolCaller over the same transport `exec_tool` uses: headless → the
// in-process runtime; attach → a BridgeClient. Construct once per run; the
// transport it points at outlives it on run()'s stack.
class CliToolCaller : public ai_strategy::ToolCaller {
  public:
    CliToolCaller(bool headless, headless::HeadlessRuntime* hr, BridgeClient* client)
        : headless_(headless), hr_(hr), client_(client) {}

    mcp::ToolResult call(const QString& name, const QJsonObject& args) override {
        if (headless_)
            return hr_->call_tool(name, args);
        return client_result_to_tool(client_->call_tool(name, args));
    }

  private:
    bool headless_;
    headless::HeadlessRuntime* hr_;
    BridgeClient* client_;
};

// CompletionFn wrapping the app's real LlmService. Fail-safe by construction:
//  - no active provider configured → return "" (the strategy proposes nothing).
//  - any LlmService error/exception → return "" (never crash the loop).
// use_tools=false: we want a single JSON-array completion, not a tool-call loop
// (which would nest McpProvider's worker-thread dispatch inside chat()'s own
// event loop — wrong shape AND a reentrancy hazard here).
ai_strategy::LlmStrategy::CompletionFn make_real_completion_fn() {
    return [](const QString& prompt) -> QString {
        if (LlmConfigRepository::instance().get_active_provider().is_err())
            return QString();
        try {
            const ai_chat::LlmResponse resp =
                ai_chat::LlmService::instance().chat(prompt, {}, /*use_tools=*/false);
            return resp.success ? resp.content : QString();
        } catch (...) {
            return QString();
        }
    };
}

int ai_usage() {
    std::fprintf(stderr,
                 "usage: ai run strategy <meanrev|claude> --mode paper "
                 "[--interval-sec N] [--max-iters M] [--duration-sec D] [--symbols A,B,C]\n");
    return 2;
}

int ai_ctx_usage() {
    std::fprintf(stderr, "usage: ai ctx <symbol> [--json] [--market prediction|equity]\n");
    return 2;
}

int ai_screen_usage() {
    std::fprintf(stderr,
                 "usage: ai screen [--market prediction|equity|crypto] [--limit N] [--json]\n");
    return 2;
}

}  // namespace

int ai_run_strategy(const GlobalOpts& opts, const QStringList& rest) {
    if (rest.isEmpty())
        return ai_usage();

    QStringList args = rest;
    const QString name = args.takeFirst();

    QString mode = QStringLiteral("paper");
    int interval_sec = 15;
    int max_iters = 0;
    int duration_sec = 0;
    QStringList symbols;

    // Consume the next token as the value for `flag`; false if missing.
    auto take_val = [&](const QString& flag, QString& dst) -> bool {
        if (args.isEmpty()) {
            std::fprintf(stderr, "error: %s requires a value\n", qUtf8Printable(flag));
            return false;
        }
        dst = args.takeFirst();
        return true;
    };

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        QString v;
        if (f == "--mode") {
            if (!take_val(f, v)) return 2;
            mode = v;
        } else if (f == "--interval-sec") {
            if (!take_val(f, v)) return 2;
            interval_sec = v.toInt();
        } else if (f == "--max-iters") {
            if (!take_val(f, v)) return 2;
            max_iters = v.toInt();
        } else if (f == "--duration-sec") {
            if (!take_val(f, v)) return 2;
            duration_sec = v.toInt();
        } else if (f == "--symbols") {
            if (!take_val(f, v)) return 2;
            symbols = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_usage();
        }
    }

    if (symbols.isEmpty())
        symbols = QStringList{QStringLiteral("AAPL")};

    // Paper-only: the loop drives ONLY the gated paper substrate. Refuse live.
    if (mode == QLatin1String("live")) {
        std::fprintf(stderr, "live strategy loop not supported (paper-first)\n");
        return 2;
    }
    if (mode != QLatin1String("paper")) {
        std::fprintf(stderr, "error: unknown --mode '%s' (only 'paper' is supported)\n",
                     qUtf8Printable(mode));
        return 2;
    }

    // Instantiate the named strategy.
    std::unique_ptr<ai_strategy::Strategy> strategy;
    if (name == QLatin1String("meanrev")) {
        strategy = std::make_unique<ai_strategy::MeanReversionStrategy>(symbols);
    } else if (name == QLatin1String("claude")) {
        strategy = std::make_unique<ai_strategy::LlmStrategy>(symbols, make_real_completion_fn());
    } else {
        std::fprintf(stderr, "error: unknown strategy '%s' (meanrev|claude)\n",
                     qUtf8Printable(name));
        return 2;
    }

    // Bring up the transport (own it on the stack; init is idempotent and the
    // `ai` path is the only thing initing in this process).
    headless::HeadlessRuntime hr;
    std::optional<BridgeClient> client;
    if (opts.headless) {
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    } else {
        auto r = resolve(opts.profile);
        if (auto* d = std::get_if<Discovered>(&r)) {
            client = BridgeClient(d->info);
        } else {
            std::fprintf(stderr, "%s (profile '%s')\n",
                         describe(std::get<DiscoveryError>(r)), qUtf8Printable(opts.profile));
            return 3;
        }
    }
    CliToolCaller caller(opts.headless, &hr, client ? &*client : nullptr);

    // SIGINT → clean stop after the current tick.
    g_stop.store(false);
    std::signal(SIGINT, on_sigint);

    ai_strategy::StrategyRunner runner;
    runner.on_log = [](const QString& msg) {
        std::printf("[strategy] %s\n", msg.toUtf8().constData());
        std::fflush(stdout);
    };

    ai_strategy::RunConfig cfg;
    cfg.interval_sec = interval_sec;
    cfg.max_iters = max_iters;
    cfg.duration_sec = duration_sec;

    std::printf("[strategy] running '%s' mode=paper interval=%ds max-iters=%d duration=%ds symbols=%s\n",
                qUtf8Printable(strategy->name()), interval_sec, max_iters, duration_sec,
                qUtf8Printable(symbols.join(QLatin1Char(','))));
    std::fflush(stdout);

    const ai_strategy::RunSummary s =
        runner.run(*strategy, caller, cfg, [] { return g_stop.load(); });

    // Final machine-greppable summary line (contains "ticks" and "halted").
    std::printf("summary: ticks=%d proposed=%d prepared=%d filled=%d rejected=%d errors=%d halted=%s\n",
                s.ticks, s.proposed, s.prepared, s.filled, s.rejected, s.errors,
                s.halted_by_kill_switch ? "true" : "false");
    std::fflush(stdout);

    // Restore default SIGINT so a follow-up Ctrl-C behaves normally.
    std::signal(SIGINT, SIG_DFL);
    return 0;
}

int ai_ctx_command(const GlobalOpts& opts, const QStringList& rest) {
    if (rest.isEmpty())
        return ai_ctx_usage();

    QStringList args = rest;
    const QString symbol = args.takeFirst();
    if (symbol.isEmpty() || symbol.startsWith(QLatin1String("--")))
        return ai_ctx_usage();

    QString market;
    bool json_flag = false;
    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--market")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --market requires a value\n");
                return 2;
            }
            market = args.takeFirst();
            if (market != QLatin1String("prediction") && market != QLatin1String("equity")) {
                std::fprintf(stderr, "error: --market must be 'prediction' or 'equity'\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_ctx_usage();
        }
    }

    // Bring up the DB only if it isn't already live in this process. A real
    // CLI invocation is a fresh process, so this is always false there and
    // init() (same local-HeadlessRuntime bring-up ai_run_strategy uses above)
    // runs exactly once. The guard exists for hosts (e.g. the dispatch test
    // binary) that already brought the DB up earlier in-process: constructing
    // a second HeadlessRuntime and calling init() there would tear down and
    // replace the live "openmarketterminal_main" QSqlDatabase connection out
    // from under whatever already opened it.
    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: assess() issues only SELECTs (see DecisionContext.h's READ-
    // ONLY INVARIANT) -- this command places no order, writes no cli.* gate
    // setting, and performs no DB write of any kind.
    const auto packet = ai_decision::assess(symbol, market);
    QJsonObject obj = ai_decision::to_json(packet);

    // READ-ONLY: floor_verdict is pure (no DB, no I/O) -- it just reads the
    // already-assessed packet's edge signals. This adds a floor field to the
    // CLI's own JSON, not to DecisionPacket/to_json, so ai_decision does not
    // gain an ai_strategy dependency (layering: cli -> {ai_decision, ai_strategy}).
    const ai_strategy::GateVerdict floor = ai_strategy::floor_verdict(
        ai_strategy::FloorInputs{packet.has_edge_signal, packet.gate, packet.clears_cost, packet.freshness},
        ai_strategy::FloorPolicy{true});
    obj.insert(QStringLiteral("floor_permits"), floor.ok);
    obj.insert(QStringLiteral("floor_reason"), floor.ok ? QString() : floor.reason);

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("symbol:              %s\n", qUtf8Printable(packet.symbol));
    if (!packet.market.isEmpty())
        std::printf("market:              %s\n", qUtf8Printable(packet.market));
    std::printf("has_edge_signal:     %s\n", packet.has_edge_signal ? "true" : "false");
    if (packet.has_edge_signal) {
        std::printf("venue:               %s\n", qUtf8Printable(packet.venue));
        std::printf("call/side/gate:      %s / %s / %s\n", qUtf8Printable(packet.call),
                    qUtf8Printable(packet.side), qUtf8Printable(packet.gate));
        std::printf("edge_after_cost:     %.4f\n", packet.edge_after_cost);
        std::printf("round_trip_cost_bps: %.4f\n", packet.round_trip_cost_bps);
    }
    std::printf("clears_cost:         %s\n", qUtf8Printable(packet.clears_cost));
    std::printf("freshness:           %s\n", qUtf8Printable(packet.freshness));
    std::printf("lane_verdict:        %s\n", qUtf8Printable(packet.lane_verdict));
    std::printf("recommendation_hint: %s\n", qUtf8Printable(packet.recommendation_hint));
    std::printf("floor:               %s\n",
                floor.ok ? "permit"
                         : qUtf8Printable(QStringLiteral("skip (%1)").arg(floor.reason)));
    std::printf("position:            %s qty=%.4f\n", qUtf8Printable(packet.position_source),
                packet.position_qty);
    return 0;
}

int ai_screen_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString market;
    int limit = 5;
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--market")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --market requires a value\n");
                return 2;
            }
            market = args.takeFirst();
            if (market != QLatin1String("prediction") && market != QLatin1String("equity") &&
                market != QLatin1String("crypto")) {
                std::fprintf(stderr, "error: --market must be 'prediction', 'equity', or 'crypto'\n");
                return 2;
            }
        } else if (f == QLatin1String("--limit")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --limit requires a value\n");
                return 2;
            }
            const QString v = args.takeFirst();
            bool ok = false;
            const int parsed = v.toInt(&ok);
            if (!ok || parsed <= 0) {
                std::fprintf(stderr, "error: --limit must be a positive integer\n");
                return 2;
            }
            limit = parsed;
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_screen_usage();
        }
    }

    // Bring up the DB only if it isn't already live in this process -- same
    // idempotent-init guard as ai_ctx_command above (see its comment).
    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: screen() issues only SELECTs, directly and via assess() (see
    // Screener.h's READ-ONLY INVARIANT) -- this command places no order,
    // writes no cli.* gate setting, and performs no DB write of any kind.
    const QVector<ai_decision::ScreenRow> rows = ai_decision::screen(market, limit);
    const QJsonArray arr = ai_decision::screen_to_json(rows);

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(arr).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    if (rows.isEmpty()) {
        std::printf("(no candidates)\n");
        return 0;
    }
    std::printf("%-16s %-11s %10s  %-6s %-10s %s\n", "SYMBOL", "MARKET", "EDGE", "SIDE",
                "HORIZON", "FRESHNESS");
    for (const auto& row : rows) {
        std::printf("%-16s %-11s %10.4f  %-6s %-10s %s\n", qUtf8Printable(row.symbol),
                    qUtf8Printable(row.market), row.edge_after_cost, qUtf8Printable(row.side),
                    qUtf8Printable(row.horizon), qUtf8Printable(row.freshness));
    }
    return 0;
}

int ai_positions_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString handler;
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --handler requires a value\n");
                return 2;
            }
            handler = args.takeFirst();
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return 2;
        }
    }

    // Bring up the DB only if it isn't already live in this process -- same
    // idempotent-init guard as ai_ctx_command above (see its comment).
    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: positions_of() folds ai_fill via SELECTs only (see
    // AiLedger.h) -- this command writes no ai_fill row, no cli.* gate
    // setting, and performs no DB write of any kind.
    const QVector<ai_ledger::HandlerPosition> ps = ai_ledger::positions_of(handler);

    QJsonArray arr;
    for (const auto& hp : ps) {
        arr.append(QJsonObject{{"handler", hp.handler},
                               {"symbol", hp.symbol},
                               {"net_qty", hp.position.net_qty},
                               {"avg_entry_price", hp.position.avg_entry_price},
                               {"realized_pnl", hp.position.realized_pnl}});
    }

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(arr).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    for (const auto& hp : ps) {
        std::printf("%-16s %-16s net=%.4f avg=%.4f realized=%.4f\n", qUtf8Printable(hp.handler),
                    qUtf8Printable(hp.symbol), hp.position.net_qty, hp.position.avg_entry_price,
                    hp.position.realized_pnl);
    }
    return 0;
}

int ai_pnl_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString handler;
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --handler requires a value\n");
                return 2;
            }
            handler = args.takeFirst();
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return 2;
        }
    }

    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: realized_total()/positions_of() issue only SELECTs (see
    // AiLedger.h) -- this command writes no ai_fill row, no cli.* gate
    // setting, and performs no DB write of any kind.
    const double realized = ai_ledger::realized_total(handler);
    const QVector<ai_ledger::HandlerPosition> ps = ai_ledger::positions_of(handler);

    QJsonArray open_positions;
    for (const auto& hp : ps) {
        open_positions.append(QJsonObject{{"handler", hp.handler},
                                          {"symbol", hp.symbol},
                                          {"net_qty", hp.position.net_qty},
                                          {"avg_entry_price", hp.position.avg_entry_price}});
    }
    const QJsonObject obj{{"realized_pnl", realized}, {"open_positions", open_positions}};

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("realized_pnl=%.4f open_positions=%d\n", realized, static_cast<int>(ps.size()));
    return 0;
}

int ai_ledger_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString handler;
    QString symbol;
    int limit = 50;
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --handler requires a value\n");
                return 2;
            }
            handler = args.takeFirst();
        } else if (f == QLatin1String("--symbol")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --symbol requires a value\n");
                return 2;
            }
            symbol = args.takeFirst();
        } else if (f == QLatin1String("--limit")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --limit requires a value\n");
                return 2;
            }
            const QString v = args.takeFirst();
            bool ok = false;
            const int parsed = v.toInt(&ok);
            if (!ok) {
                std::fprintf(stderr, "error: --limit must be an integer\n");
                return 2;
            }
            limit = parsed;
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return 2;
        }
    }

    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: AiFillRepository::list() issues only a SELECT (recent
    // first) -- this command writes no ai_fill row, no cli.* gate setting,
    // and performs no DB write of any kind.
    const auto rows = AiFillRepository::instance().list(handler, symbol, limit);

    QJsonArray arr;
    if (rows.is_ok()) {
        for (const AiFill& f : rows.value()) {
            arr.append(QJsonObject{{"id", f.id},
                                   {"handler", f.handler},
                                   {"symbol", f.symbol},
                                   {"side", f.side},
                                   {"quantity", f.quantity},
                                   {"fill_price", f.fill_price},
                                   {"fee", f.fee},
                                   {"realized_pnl", f.realized_pnl},
                                   {"ts", static_cast<double>(f.ts)},
                                   {"draft_id", f.draft_id}});
        }
    }

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(arr).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        std::printf("%-16s %-16s %-4s qty=%.4f px=%.4f pnl=%.4f\n",
                    qUtf8Printable(o.value("handler").toString()),
                    qUtf8Printable(o.value("symbol").toString()),
                    qUtf8Printable(o.value("side").toString()), o.value("quantity").toDouble(),
                    o.value("fill_price").toDouble(), o.value("realized_pnl").toDouble());
    }
    return 0;
}

int ai_scorecard_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString handler;
    QString symbol;
    int limit = 0;  // 0 = all closes
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) { std::fprintf(stderr, "error: --handler requires a value\n"); return 2; }
            handler = args.takeFirst();
        } else if (f == QLatin1String("--symbol")) {
            if (args.isEmpty()) { std::fprintf(stderr, "error: --symbol requires a value\n"); return 2; }
            symbol = args.takeFirst();
        } else if (f == QLatin1String("--limit")) {
            if (args.isEmpty()) { std::fprintf(stderr, "error: --limit requires a value\n"); return 2; }
            bool ok = false;
            const int parsed = args.takeFirst().toInt(&ok);
            limit = (ok && parsed > 0) ? parsed : 0;  // non-positive/garbage -> all
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return 2;
        }
    }

    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // READ-ONLY: scorecard_of only SELECTs (AiFillRepository::list) -- no write of any kind.
    const ai_ledger::Scorecard sc = ai_ledger::scorecard_of(handler, symbol, limit);
    const QJsonObject obj = ai_ledger::scorecard_to_json(sc);

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("trades=%d wins=%d losses=%d hit_rate=%.4f realized=%.4f avg=%.4f best=%.4f worst=%.4f\n",
                sc.trades, sc.wins, sc.losses, sc.hit_rate, sc.realized_total, sc.avg_realized,
                sc.best, sc.worst);
    for (const ai_ledger::SymbolScore& ss : sc.per_symbol)
        std::printf("  %s trades=%d hit_rate=%.4f realized=%.4f\n", qUtf8Printable(ss.symbol),
                    ss.trades, ss.hit_rate, ss.realized_total);
    return 0;
}

int ai_record_fill_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    QString handler;
    QString symbol;
    QString side;
    QString draft_id;
    QString qty_str;
    QString price_str;
    QString fee_str;
    bool json_flag = false;

    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --handler requires a value\n");
                return 2;
            }
            handler = args.takeFirst();
        } else if (f == QLatin1String("--symbol")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --symbol requires a value\n");
                return 2;
            }
            symbol = args.takeFirst();
        } else if (f == QLatin1String("--side")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --side requires a value\n");
                return 2;
            }
            side = args.takeFirst();
        } else if (f == QLatin1String("--qty")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --qty requires a value\n");
                return 2;
            }
            qty_str = args.takeFirst();
        } else if (f == QLatin1String("--price")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --price requires a value\n");
                return 2;
            }
            price_str = args.takeFirst();
        } else if (f == QLatin1String("--fee")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --fee requires a value\n");
                return 2;
            }
            fee_str = args.takeFirst();
        } else if (f == QLatin1String("--draft-id")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --draft-id requires a value\n");
                return 2;
            }
            draft_id = args.takeFirst();
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return 2;
        }
    }

    auto usage = [] {
        std::fprintf(stderr,
                     "usage: ai record-fill --handler H --symbol S --side buy|sell --qty Q "
                     "--price P [--fee F] [--draft-id D] [--json]\n");
        return 2;
    };

    bool qty_ok = false;
    bool price_ok = false;
    bool fee_ok = false;
    const double qty = qty_str.toDouble(&qty_ok);
    const double price = price_str.toDouble(&price_ok);
    const double fee = fee_str.isEmpty() ? 0.0 : fee_str.toDouble(&fee_ok);

    if (handler.isEmpty() || symbol.isEmpty() ||
        (side != QLatin1String("buy") && side != QLatin1String("sell")) ||
        !qty_ok || qty <= 0.0 || !price_ok || price <= 0.0 ||
        (!fee_str.isEmpty() && (!fee_ok || fee < 0.0))) {
        return usage();
    }

    // Bring up the DB only if it isn't already live in this process -- same
    // idempotent-init guard as ai_ctx_command above (see its comment).
    if (!openmarketterminal::Database::instance().is_open()) {
        headless::HeadlessRuntime hr;
        auto ir = hr.init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            return 7;
        }
    }

    // PAPER-ONLY WRITE: the ONLY mutation performed here is the single
    // ai_fill row appended by record_fill() -- never a cli.* gate setting,
    // never a live order.
    auto rec = ai_ledger::record_fill(handler, symbol, side, qty, price, fee, draft_id);
    if (rec.is_err()) {
        std::fprintf(stderr, "record-fill failed: %s\n", rec.error().c_str());
        return 2;
    }
    const AiFill& f = rec.value();
    const ai_ledger::LedgerPosition p = ai_ledger::position_of(handler, symbol);

    const QJsonObject obj{
        {"fill", QJsonObject{{"id", f.id},
                             {"handler", f.handler},
                             {"symbol", f.symbol},
                             {"side", f.side},
                             {"quantity", f.quantity},
                             {"fill_price", f.fill_price},
                             {"fee", f.fee},
                             {"realized_pnl", f.realized_pnl},
                             {"ts", static_cast<double>(f.ts)},
                             {"draft_id", f.draft_id}}},
        {"position", QJsonObject{{"net_qty", p.net_qty},
                                 {"avg_entry_price", p.avg_entry_price},
                                 {"realized_pnl", p.realized_pnl}}}};

    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("recorded %s %s %s qty=%.4f px=%.4f realized=%.4f -> net=%.4f\n",
                qUtf8Printable(handler), qUtf8Printable(symbol), qUtf8Printable(side), qty, price,
                f.realized_pnl, p.net_qty);
    return 0;
}

}  // namespace openmarketterminal::cli
