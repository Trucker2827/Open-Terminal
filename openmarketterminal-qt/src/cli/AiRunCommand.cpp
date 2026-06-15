// AiRunCommand.cpp — see AiRunCommand.h.
#include "cli/AiRunCommand.h"

#include "cli/BridgeClient.h"
#include "cli/BridgeDiscovery.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpTypes.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/LlmConfigRepository.h"

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

}  // namespace openmarketterminal::cli
