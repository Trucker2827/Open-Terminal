// AiRunCommand.cpp — see AiRunCommand.h.
#include "cli/AiRunCommand.h"

#include "cli/BridgeClient.h"
#include "cli/BridgeDiscovery.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpTypes.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/StrategyRegistry.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/AiHandlerRepository.h"
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

QJsonObject strategy_info_to_json(const ai_strategy::StrategyInfo& info) {
    return QJsonObject{
        {"name", info.name},
        {"description", info.description},
        {"needs_provider", info.needs_provider},
    };
}

QJsonObject ai_handler_to_json(const AiHandler& h) {
    return QJsonObject{
        {"name", h.name},
        {"strategy", h.strategy},
        {"provider", h.provider},
        {"symbols", h.symbols},
        {"interval_sec", h.interval_sec},
        {"allowed_venues", h.allowed_venues},
        {"max_notional", h.max_notional},
        {"max_position", h.max_position},
        {"enabled", h.enabled},
        {"notes", h.notes},
        {"created_at", h.created_at},
    };
}

void print_ai_handler_row(const AiHandler& h) {
    std::printf("%-20s %-10s %-10s %-20s %6d %-8s %s\n", qUtf8Printable(h.name),
                qUtf8Printable(h.strategy), qUtf8Printable(h.provider), qUtf8Printable(h.symbols),
                h.interval_sec, h.enabled ? "enabled" : "disabled", qUtf8Printable(h.notes));
}

int ai_handler_usage() {
    std::fprintf(stderr,
                 "usage: ai handler create <name> --strategy <s> [--provider p] [--symbols A,B] "
                 "[--interval-sec N] [--venues v1,v2] [--max-notional X] [--max-position Y] "
                 "[--notes \"...\"]\n"
                 "       ai handler list [--json]\n"
                 "       ai handler show <name> [--json]\n"
                 "       ai handler delete <name>\n"
                 "       ai handler enable <name>\n"
                 "       ai handler disable <name>\n");
    return 2;
}

// Bring up the shared DB so AiHandlerRepository has a live connection
// regardless of --headless. Skips HeadlessRuntime::init() entirely when the
// DB is already open: HeadlessRuntime::init()'s "second call is a no-op"
// idempotency is tracked on the INSTANCE (`initialized_`), and this
// translation unit can't see CommandDispatch.cpp's own file-static
// headless_runtime() singleton that other `ai ...` commands (and most other
// command groups) already bring the DB up through. Re-running full bring-up
// from a second HeadlessRuntime instance re-opens Database::instance() —
// re-registering the same QSqlDatabase connection name logs a "duplicate
// connection name" warning to stdout that corrupts whatever JSON that
// command was printing. Checking is_open() first makes this safe both
// stand-alone (first `ai handler ...` call in the process opens it) and
// after any other DB-touching command already opened it.
bool init_db_for_handler_command(const GlobalOpts& opts) {
    if (Database::instance().is_open())
        return true;
    static headless::HeadlessRuntime hr;
    auto ir = hr.init(opts.profile);
    if (!ir.ok) {
        std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
        return false;
    }
    return true;
}

int ai_handler_create(const GlobalOpts& opts, QStringList rest) {
    if (rest.isEmpty() || rest.first().startsWith(QLatin1String("--")))
        return ai_handler_usage();

    AiHandler h;
    h.name = rest.takeFirst();
    h.interval_sec = 60;
    h.enabled = false;  // paper-only/disarmed: a created handler is never armed.

    auto take_val = [&](const QString& flag, QString& dst) -> bool {
        if (rest.isEmpty()) {
            std::fprintf(stderr, "error: %s requires a value\n", qUtf8Printable(flag));
            return false;
        }
        dst = rest.takeFirst();
        return true;
    };

    QString strategy;
    bool have_strategy = false;
    while (!rest.isEmpty()) {
        const QString f = rest.takeFirst();
        QString v;
        if (f == "--strategy") {
            if (!take_val(f, v)) return 2;
            strategy = v;
            have_strategy = true;
        } else if (f == "--provider") {
            if (!take_val(f, v)) return 2;
            h.provider = v;
        } else if (f == "--symbols") {
            if (!take_val(f, v)) return 2;
            h.symbols = v;
        } else if (f == "--interval-sec") {
            if (!take_val(f, v)) return 2;
            h.interval_sec = v.toInt();
        } else if (f == "--venues") {
            if (!take_val(f, v)) return 2;
            h.allowed_venues = v;
        } else if (f == "--max-notional") {
            if (!take_val(f, v)) return 2;
            h.max_notional = v.toDouble();
        } else if (f == "--max-position") {
            if (!take_val(f, v)) return 2;
            h.max_position = v.toDouble();
        } else if (f == "--notes") {
            if (!take_val(f, v)) return 2;
            h.notes = v;
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_handler_usage();
        }
    }

    if (!have_strategy || strategy.isEmpty()) {
        std::fprintf(stderr, "error: --strategy is required\n");
        return ai_handler_usage();
    }

    // Validate the strategy name against the plugin registry BEFORE writing
    // any row: an unknown strategy must leave the table untouched.
    ai_strategy::StrategyRegistry registry;
    ai_strategy::register_builtin_strategies(registry);
    if (!registry.has(strategy)) {
        std::fprintf(stderr, "error: unknown strategy '%s' (see 'ai strategy list')\n",
                     qUtf8Printable(strategy));
        return 2;
    }
    h.strategy = strategy;

    if (!init_db_for_handler_command(opts))
        return 7;

    auto saved = AiHandlerRepository::instance().save(h);
    if (saved.is_err()) {
        std::fprintf(stderr, "error: failed to save handler: %s\n", saved.error().c_str());
        return 5;
    }
    // Re-fetch so the printed row reflects what's actually persisted
    // (created_at is set server-side by the INSERT).
    auto fetched = AiHandlerRepository::instance().get(h.name);
    if (fetched.is_err()) {
        std::fprintf(stderr, "error: handler saved but re-fetch failed: %s\n", fetched.error().c_str());
        return 5;
    }

    if (opts.json) {
        std::printf("%s\n", QJsonDocument(ai_handler_to_json(fetched.value()))
                                .toJson(QJsonDocument::Compact)
                                .constData());
    } else {
        std::printf("created handler '%s' (strategy=%s, disabled)\n", qUtf8Printable(h.name),
                    qUtf8Printable(h.strategy));
    }
    return 0;
}

int ai_handler_list(const GlobalOpts& opts) {
    if (!init_db_for_handler_command(opts))
        return 7;
    auto listed = AiHandlerRepository::instance().list();
    if (listed.is_err()) {
        std::fprintf(stderr, "error: failed to list handlers: %s\n", listed.error().c_str());
        return 5;
    }
    if (opts.json) {
        QJsonArray arr;
        for (const auto& h : listed.value()) arr.append(ai_handler_to_json(h));
        std::printf("%s\n", QJsonDocument(QJsonObject{{"handlers", arr}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }
    std::printf("%-20s %-10s %-10s %-20s %6s %-8s %s\n", "name", "strategy", "provider", "symbols",
                "ivl-s", "state", "notes");
    for (const auto& h : listed.value()) print_ai_handler_row(h);
    return 0;
}

int ai_handler_show(const GlobalOpts& opts, const QStringList& rest) {
    if (rest.size() != 1)
        return ai_handler_usage();
    if (!init_db_for_handler_command(opts))
        return 7;
    auto fetched = AiHandlerRepository::instance().get(rest.first());
    if (fetched.is_err()) {
        std::fprintf(stderr, "error: handler not found: %s\n", qUtf8Printable(rest.first()));
        return 4;
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(ai_handler_to_json(fetched.value()))
                                .toJson(QJsonDocument::Compact)
                                .constData());
    } else {
        print_ai_handler_row(fetched.value());
    }
    return 0;
}

int ai_handler_delete(const GlobalOpts& opts, const QStringList& rest) {
    if (rest.size() != 1)
        return ai_handler_usage();
    if (!init_db_for_handler_command(opts))
        return 7;
    // Existence check first so deleting an unknown name is a clear error
    // rather than a silent no-op success (DELETE with no match still "ok"s).
    if (AiHandlerRepository::instance().get(rest.first()).is_err()) {
        std::fprintf(stderr, "error: handler not found: %s\n", qUtf8Printable(rest.first()));
        return 4;
    }
    auto removed = AiHandlerRepository::instance().remove(rest.first());
    if (removed.is_err()) {
        std::fprintf(stderr, "error: failed to delete handler: %s\n", removed.error().c_str());
        return 5;
    }
    std::printf("deleted handler '%s'\n", qUtf8Printable(rest.first()));
    return 0;
}

// Flips ONLY the saved handler config's `enabled` column
// (AiHandlerRepository::set_enabled). This never arms live trading: there is
// no live execution path in this plugin core, and no cli.* SettingsGate flag
// is read or written here.
int ai_handler_set_enabled(const GlobalOpts& opts, const QStringList& rest, bool enabled) {
    if (rest.size() != 1)
        return ai_handler_usage();
    if (!init_db_for_handler_command(opts))
        return 7;
    if (AiHandlerRepository::instance().get(rest.first()).is_err()) {
        std::fprintf(stderr, "error: handler not found: %s\n", qUtf8Printable(rest.first()));
        return 4;
    }
    auto updated = AiHandlerRepository::instance().set_enabled(rest.first(), enabled);
    if (updated.is_err()) {
        std::fprintf(stderr, "error: failed to update handler: %s\n", updated.error().c_str());
        return 5;
    }
    std::printf("%s handler '%s'\n", enabled ? "enabled" : "disabled", qUtf8Printable(rest.first()));
    return 0;
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

int ai_strategy_list_command(const GlobalOpts& opts) {
    ai_strategy::StrategyRegistry registry;
    ai_strategy::register_builtin_strategies(registry);
    const QVector<ai_strategy::StrategyInfo> infos = registry.list();

    if (opts.json) {
        QJsonArray arr;
        for (const auto& info : infos) arr.append(strategy_info_to_json(info));
        std::printf("%s\n", QJsonDocument(QJsonObject{{"strategies", arr}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }

    std::printf("%-10s %-14s %s\n", "name", "needs_provider", "description");
    for (const auto& info : infos) {
        std::printf("%-10s %-14s %s\n", qUtf8Printable(info.name), info.needs_provider ? "yes" : "no",
                    qUtf8Printable(info.description));
    }
    return 0;
}

int ai_handler_command(const GlobalOpts& opts, const QString& action, QStringList rest) {
    if (action == QLatin1String("create"))
        return ai_handler_create(opts, std::move(rest));
    if (action == QLatin1String("list"))
        return ai_handler_list(opts);
    if (action == QLatin1String("show"))
        return ai_handler_show(opts, rest);
    if (action == QLatin1String("delete") || action == QLatin1String("remove"))
        return ai_handler_delete(opts, rest);
    if (action == QLatin1String("enable"))
        return ai_handler_set_enabled(opts, rest, /*enabled=*/true);
    if (action == QLatin1String("disable"))
        return ai_handler_set_enabled(opts, rest, /*enabled=*/false);
    return ai_handler_usage();
}

}  // namespace openmarketterminal::cli
