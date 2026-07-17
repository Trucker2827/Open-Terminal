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
#include "mcp/tools/SettingsGate.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"
#include "services/ai_strategy/StrategyRegistry.h"
#include "services/ai_strategy/StrategyRunner.h"
#include "services/ai_strategy/TypedAction.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/AiFillRepository.h"
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
                 "[--market crypto|equity|prediction] [--interval-sec N] [--max-iters M] "
                 "[--duration-sec D] [--symbols A,B,C] [--no-floor] "
                 "[--max-aggregate-qty N] [--max-position-qty N] [--max-notional-per-order N]\n"
                 "  --market is REQUIRED for the 'claude' strategy (edge-scoped direction).\n");
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
                 "       ai handler disable <name>\n"
                 "       ai handler status [<name>] [--json]\n"
                 "       ai handler run <name> --paper [--max-iters N] [--duration-sec D] "
                 "[--interval-sec N]\n");
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

// `ai handler status [<name>] [--json]` — READ-ONLY arm-state readout.
//
// SAFETY-CRITICAL / READ-ONLY: this command only READS gate flags through the
// SettingsGate accessors (mcp::cli_*). It NEVER calls any settings/gate SETTER
// and constructs nothing that writes a cli.* value. `armed` is hard-coded false
// because there is NO live execution path in this plugin core; disarmed_reason
// enumerates the conditions that would additionally block live if one existed.
int ai_handler_status(const GlobalOpts& opts, const QStringList& rest) {
    // Optional positional <name>; ignore any stray flags (globals are stripped).
    QString name;
    for (const QString& a : rest) {
        if (!a.startsWith(QLatin1String("--"))) {
            name = a;
            break;
        }
    }

    if (!init_db_for_handler_command(opts))
        return 7;

    // READ-ONLY gate reads — no setter is ever invoked on this path.
    const bool kill = mcp::cli_kill_switch_engaged();
    const bool paper_ok = mcp::cli_paper_trading_allowed();
    const bool trading_ok = mcp::cli_trading_allowed();
    const bool live_armed = mcp::cli_live_armed();
    const bool fast_armed = mcp::cli_fast_live_armed();
    const QStringList venues = mcp::cli_allowed_venues();

    auto prov = LlmConfigRepository::instance().get_active_provider();
    const QString provider = prov.is_ok() ? prov.value().provider : QStringLiteral("none");

    QJsonArray reasons;
    reasons.append(QStringLiteral("no live path in plugin core"));
    if (!live_armed) reasons.append(QStringLiteral("live_trading_armed=false"));
    if (!trading_ok) reasons.append(QStringLiteral("allow_trading=false"));
    if (!fast_armed) reasons.append(QStringLiteral("fast_live_armed=false"));
    if (kill) reasons.append(QStringLiteral("kill_switch=engaged"));

    QJsonObject gates{
        {"kill_switch", kill},
        {"allow_paper_trading", paper_ok},
        {"allow_trading", trading_ok},
        {"live_trading_armed", live_armed},
        {"fast_live_armed", fast_armed},
        {"allowed_venues", QJsonArray::fromStringList(venues)},
    };

    QJsonObject out{
        {"provider", provider},
        {"gates", gates},
        {"armed", false},
        {"disarmed_reason", reasons},
    };

    if (!name.isEmpty()) {
        auto fetched = AiHandlerRepository::instance().get(name);
        if (fetched.is_err()) {
            std::fprintf(stderr, "error: handler not found: %s\n", qUtf8Printable(name));
            return 4;
        }
        out.insert(QStringLiteral("handler"), ai_handler_to_json(fetched.value()));
    }

    if (opts.json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("armed:    false\n");
    std::printf("provider: %s\n", qUtf8Printable(provider));
    std::printf("gates:    kill_switch=%s allow_paper_trading=%s allow_trading=%s "
                "live_trading_armed=%s fast_live_armed=%s allowed_venues=%s\n",
                kill ? "true" : "false", paper_ok ? "true" : "false", trading_ok ? "true" : "false",
                live_armed ? "true" : "false", fast_armed ? "true" : "false",
                qUtf8Printable(venues.join(QLatin1Char(','))));
    if (!name.isEmpty())
        print_ai_handler_row(AiHandlerRepository::instance().get(name).value());
    std::printf("disarmed_reason:");
    for (const QJsonValue& v : reasons) std::printf(" [%s]", qUtf8Printable(v.toString()));
    std::printf("\n");
    return 0;
}

// `ai handler run <name> --paper [--max-iters N|--duration-sec D|--interval-sec N]`
//
// SAFETY-CRITICAL / PAPER-ONLY: loads the saved handler, builds its strategy via
// the registry, and drives the SAME StrategyRunner paper path as
// `ai run strategy`. It REFUSES any mode other than `paper` (exit 2) — there is
// NO live execution path. RunConfig carries no notional/position caps, so the
// handler's stored caps are surfaced as stored-not-enforced in the summary line.
int ai_handler_run(const GlobalOpts& opts, QStringList rest) {
    if (rest.isEmpty() || rest.first().startsWith(QLatin1String("--")))
        return ai_handler_usage();
    const QString name = rest.takeFirst();

    QString mode = QStringLiteral("paper");
    int interval_sec = -1;  // <0 → fall back to the handler's stored interval.
    int max_iters = 0;
    int duration_sec = 0;

    auto take_val = [&](const QString& flag, QString& dst) -> bool {
        if (rest.isEmpty()) {
            std::fprintf(stderr, "error: %s requires a value\n", qUtf8Printable(flag));
            return false;
        }
        dst = rest.takeFirst();
        return true;
    };

    while (!rest.isEmpty()) {
        const QString f = rest.takeFirst();
        QString v;
        if (f == "--paper") {
            mode = QStringLiteral("paper");
        } else if (f == "--mode") {
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
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_handler_usage();
        }
    }

    // PAPER-ONLY guard — the SINGLE source of truth for this command. Refuse any
    // non-paper mode BEFORE loading or building anything. There is no live path.
    if (mode == QLatin1String("live")) {
        std::fprintf(stderr, "live handler run not supported (paper-first)\n");
        return 2;
    }
    if (mode != QLatin1String("paper")) {
        std::fprintf(stderr, "error: unknown --mode '%s' (only 'paper' is supported)\n",
                     qUtf8Printable(mode));
        return 2;
    }

    if (!init_db_for_handler_command(opts))
        return 7;

    auto fetched = AiHandlerRepository::instance().get(name);
    if (fetched.is_err()) {
        std::fprintf(stderr, "error: handler not found: %s\n", qUtf8Printable(name));
        return 4;
    }
    const AiHandler h = fetched.value();

    ai_strategy::StrategyRegistry registry;
    ai_strategy::register_builtin_strategies(registry);
    if (!registry.has(h.strategy)) {
        std::fprintf(stderr, "error: handler references unknown strategy '%s'\n",
                     qUtf8Printable(h.strategy));
        return 2;
    }

    // needs_provider: LLM-backed strategies refuse to run without an active provider.
    bool needs_provider = false;
    for (const auto& info : registry.list())
        if (info.name == h.strategy) needs_provider = info.needs_provider;
    if (needs_provider && LlmConfigRepository::instance().get_active_provider().is_err()) {
        std::fprintf(stderr, "error: no active LLM provider (strategy '%s' needs one)\n",
                     qUtf8Printable(h.strategy));
        return 6;
    }

    QStringList symbols = h.symbols.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (symbols.isEmpty())
        symbols = QStringList{QStringLiteral("AAPL")};

    std::unique_ptr<ai_strategy::Strategy> strategy =
        registry.build(h.strategy, {symbols, make_real_completion_fn()});
    if (!strategy) {
        std::fprintf(stderr, "error: failed to build strategy '%s'\n", qUtf8Printable(h.strategy));
        return 5;
    }

    const int ivl = interval_sec >= 0 ? interval_sec : h.interval_sec;

    // Transport bring-up (mirrors ai_run_strategy). Guard the DB init: only bring
    // it up when nothing else already opened it — re-opening re-registers the main
    // connection name and corrupts stdout with a "duplicate connection" warning.
    headless::HeadlessRuntime hr;
    std::optional<BridgeClient> client;
    if (opts.headless) {
        if (!Database::instance().is_open()) {
            auto ir = hr.init(opts.profile);
            if (!ir.ok) {
                std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
                return 7;
            }
        }
    } else {
        auto r = resolve(opts.profile);
        if (auto* d = std::get_if<Discovered>(&r)) {
            client = BridgeClient(d->info);
        } else {
            std::fprintf(stderr, "%s (profile '%s')\n", describe(std::get<DiscoveryError>(r)),
                         qUtf8Printable(opts.profile));
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
    cfg.interval_sec = ivl;
    cfg.max_iters = max_iters;
    cfg.duration_sec = duration_sec;
    cfg.max_notional_per_order = h.max_notional;
    cfg.max_position_qty = h.max_position;
    for (const QString& raw_venue : h.allowed_venues.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString venue = raw_venue.trimmed().toLower();
        if (!venue.isEmpty() && !cfg.allowed_venues.contains(venue))
            cfg.allowed_venues.push_back(venue);
    }

    std::printf("[handler] running '%s' strategy=%s mode=paper interval=%ds max-iters=%d "
                "duration=%ds symbols=%s\n",
                qUtf8Printable(h.name), qUtf8Printable(h.strategy), ivl, max_iters, duration_sec,
                qUtf8Printable(symbols.join(QLatin1Char(','))));
    std::fflush(stdout);

    const ai_strategy::RunSummary s =
        runner.run(*strategy, caller, cfg, [] { return g_stop.load(); });

    std::printf("summary: ticks=%d proposed=%d prepared=%d filled=%d rejected=%d errors=%d "
                "halted=%s caps_enforced=true max_notional=%.2f max_position=%.2f allowed_venues=%s\n",
                s.ticks, s.proposed, s.prepared, s.filled, s.rejected, s.errors,
                s.halted_by_kill_switch ? "true" : "false", cfg.max_notional_per_order,
                cfg.max_position_qty,
                qUtf8Printable(cfg.allowed_venues.join(QLatin1Char(','))));
    std::fflush(stdout);

    std::signal(SIGINT, SIG_DFL);
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
    bool require_floor = true;  ///< default-ON deterministic floor; --no-floor disables it.
    double max_aggregate_qty = 0.0;
    double max_position_qty = 0.0;
    double max_notional_per_order = 0.0;
    QString market;  ///< empty = unscoped (all venues); validated below when non-empty.

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
        } else if (f == "--no-floor") {
            require_floor = false;  // opt out of the default-ON deterministic floor (still paper-only).
        } else if (f == "--max-aggregate-qty") {
            if (!take_val(f, v)) return 2;
            max_aggregate_qty = v.toDouble();  // non-numeric -> 0 = off
        } else if (f == "--max-position-qty") {
            if (!take_val(f, v)) return 2;
            max_position_qty = v.toDouble();  // non-numeric -> 0 = off
        } else if (f == "--max-notional-per-order") {
            if (!take_val(f, v)) return 2;
            max_notional_per_order = v.toDouble();  // non-numeric -> 0 = off
        } else if (f == "--market") {
            if (!take_val(f, v)) return 2;
            market = v;
            if (market != QLatin1String("crypto") && market != QLatin1String("equity") &&
                market != QLatin1String("prediction")) {
                std::fprintf(stderr, "error: --market must be 'crypto', 'equity', or 'prediction'\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "error: unknown flag '%s'\n", qUtf8Printable(f));
            return ai_usage();
        }
    }

    if (symbols.isEmpty())
        symbols = QStringList{QStringLiteral("AAPL")};

    // Edge-scoped direction is only meaningful with a market to scope by: the
    // 'claude' strategy resolves ENTER side from edge_direction_of(symbol, market)
    // (and the floor's assess_fn below), so an empty market would let a
    // cross-venue row for the same symbol decide the direction (the #38 bug).
    // 'meanrev' is deterministic and never reads edge direction, so it's unaffected.
    if (name == QLatin1String("claude") && market.isEmpty()) {
        std::fprintf(stderr,
                     "error: --market <crypto|equity|prediction> is required for the 'claude' "
                     "strategy (edge-scoped direction)\n");
        return 2;
    }

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
        strategy = std::make_unique<ai_strategy::LlmStrategy>(symbols, make_real_completion_fn(),
                                                               /*max_qty=*/10.0, market);
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
    // Scope the floor's edge lookup too: assess_fn feeds the pre-trade
    // guardrail's cost/freshness/edge inputs (StrategyRunner.h), and its
    // default is the SAME empty-market assess() the #38 bug came from. When a
    // market was given, override it to reuse F1's market_venue_filter so the
    // floor agrees with the strategy's own edge_direction_of(symbol, market).
    if (!market.isEmpty())
        runner.assess_fn = [market](const QString& sym) { return ai_decision::assess(sym, market); };

    ai_strategy::RunConfig cfg;
    cfg.interval_sec = interval_sec;
    cfg.max_iters = max_iters;
    cfg.duration_sec = duration_sec;
    cfg.require_floor = require_floor;
    cfg.max_aggregate_position_qty = max_aggregate_qty;
    cfg.max_position_qty = max_position_qty;
    cfg.max_notional_per_order = max_notional_per_order;

    std::printf("[strategy] running '%s' mode=paper market=%s interval=%ds max-iters=%d duration=%ds symbols=%s floor=%s agg_cap=%.4f pos_cap=%.4f notional_cap=%.4f\n",
                qUtf8Printable(strategy->name()), market.isEmpty() ? "any" : qUtf8Printable(market),
                interval_sec, max_iters, duration_sec,
                qUtf8Printable(symbols.join(QLatin1Char(','))), require_floor ? "on" : "off", max_aggregate_qty,
                max_position_qty, max_notional_per_order);
    std::fflush(stdout);

    const ai_strategy::RunSummary s =
        runner.run(*strategy, caller, cfg, [] { return g_stop.load(); });

    // Final machine-greppable summary line (contains "ticks" and "halted").
    // floor_skipped explains a proposed>0 / filled=0 run when the deterministic
    // floor is ON (default) and the honest edge journal endorses nothing.
    std::printf("summary: ticks=%d proposed=%d prepared=%d filled=%d rejected=%d floor_skipped=%d "
                "errors=%d halted=%s\n",
                s.ticks, s.proposed, s.prepared, s.filled, s.rejected, s.floor_skipped, s.errors,
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
    // F2: floor_permits reflects a LONG (enter) entry -- the edge must endorse AND
    // recommend the long side; a short/neutral edge correctly reports false.
    const bool floor_permits = floor.ok &&
        ai_strategy::intent_agrees_with_edge(QStringLiteral("buy"), packet.side);
    obj.insert(QStringLiteral("floor_permits"), floor_permits);
    obj.insert(QStringLiteral("floor_reason"),
               floor_permits ? QString()
                   : (floor.ok ? QStringLiteral("edge recommends opposite side") : floor.reason));

    // READ-ONLY: scorecard_of issues only SELECTs over ai_fill (see
    // Scorecard.h). Aggregate across all handlers ({} = every handler),
    // symbol-scoped -- surfaces the AI's own realized track record for this
    // symbol in the CLI's own JSON, not in DecisionPacket/to_json. This is
    // CLI-side (not in assess()) on purpose: assess() is called per-candidate
    // by `ai screen`, so putting scorecard_of there would be N queries per
    // shortlist instead of one query per `ai ctx` invocation.
    const ai_ledger::Scorecard tr = ai_ledger::scorecard_of(QString(), symbol);
    obj.insert(QStringLiteral("track_record"),
               QJsonObject{{"trades", tr.trades}, {"wins", tr.wins}, {"losses", tr.losses},
                           {"hit_rate", tr.hit_rate}, {"realized_total", tr.realized_total}});

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
    std::printf("track_record:        trades=%d hit_rate=%.4f realized=%.4f\n",
                tr.trades, tr.hit_rate, tr.realized_total);
    return 0;
}

// `ai act <symbol> <enter|trim|exit|hold> [--conviction N] [--handler H]
// [--json]` -- previews what a typed verb translates to for a symbol given
// its CURRENT ledger position AND the deterministic edge's direction (the
// same edge_direction_of() resolver LlmStrategy uses by default), so the
// preview matches what a real enter would do. READ-ONLY: position_of issues
// a SELECT, edge_direction_of()/assess() issues a SELECT, and translate_action
// is pure (no DB, no LLM) -- this command writes nothing (no cli.* gate, no
// ai_fill row, no order); it never constructs a StrategyRunner and never
// calls prepare_order/submit_order.
int ai_act_command(const GlobalOpts& opts, const QStringList& rest) {
    QStringList args = rest;
    if (args.isEmpty()) {
        std::fprintf(stderr,
                     "usage: ai act <symbol> <enter|trim|exit|hold> [--conviction N] [--handler H] "
                     "[--market crypto|equity|prediction] [--json]\n");
        return 2;
    }
    const QString symbol = args.takeFirst();
    if (args.isEmpty()) {
        std::fprintf(stderr, "error: missing <action>\n");
        return 2;
    }
    const QString action = args.takeFirst();

    ai_strategy::ActionType atype;
    if (action == QLatin1String("skip") || action == QLatin1String("hold")) atype = ai_strategy::ActionType::Skip;
    else if (action == QLatin1String("enter")) atype = ai_strategy::ActionType::Enter;
    else if (action == QLatin1String("trim"))  atype = ai_strategy::ActionType::Trim;
    else if (action == QLatin1String("exit"))  atype = ai_strategy::ActionType::Exit;
    else {
        std::fprintf(stderr, "error: unknown action '%s' (enter|trim|exit|hold)\n", qUtf8Printable(action));
        return 2;
    }

    double conviction = 1.0;
    QString handler = QStringLiteral("claude");
    QString market;  ///< empty = unscoped (all venues), matching edge_direction_of()'s default.
    bool json_flag = false;
    while (!args.isEmpty()) {
        const QString f = args.takeFirst();
        if (f == QLatin1String("--json")) {
            json_flag = true;
        } else if (f == QLatin1String("--conviction")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --conviction requires a value\n");
                return 2;
            }
            conviction = args.takeFirst().toDouble();
        } else if (f == QLatin1String("--handler")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --handler requires a value\n");
                return 2;
            }
            handler = args.takeFirst();
        } else if (f == QLatin1String("--market")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "error: --market requires a value\n");
                return 2;
            }
            market = args.takeFirst();
            if (market != QLatin1String("crypto") && market != QLatin1String("equity") &&
                market != QLatin1String("prediction")) {
                std::fprintf(stderr, "error: --market must be 'crypto', 'equity', or 'prediction'\n");
                return 2;
            }
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

    // READ-ONLY: position_of issues a SELECT; translate_action is pure. No write.
    const double net_qty = ai_ledger::position_of(handler, symbol).net_qty;
    // The deterministic edge decides direction (same resolver the runtime uses),
    // scoped by --market so the preview matches a real scoped run (empty =
    // all-venues, as before). READ-ONLY (assess=SELECT).
    const int edge_dir = ai_strategy::edge_direction_of(symbol, market);
    const auto intent = ai_strategy::translate_action(
        ai_strategy::ActionChoice{symbol, atype, conviction}, net_qty, edge_dir,
        ai_strategy::ActionParams{10.0, 0.5});

    QJsonObject obj{{"action", action}, {"symbol", symbol}, {"conviction", conviction},
                    {"current_net_qty", net_qty}, {"edge_direction", edge_dir},
                    {"intent", intent ? QJsonValue(*intent) : QJsonValue(QJsonValue::Null)}};
    if (opts.json || json_flag) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    if (intent)
        std::printf("%s %s conv=%.4f net=%.4f edge_dir=%d -> %s %.4f @ market\n", qUtf8Printable(action),
                    qUtf8Printable(symbol), conviction, net_qty, edge_dir,
                    qUtf8Printable(intent->value("side").toString()), intent->value("quantity").toDouble());
    else
        std::printf("%s %s conv=%.4f net=%.4f edge_dir=%d -> no intent\n", qUtf8Printable(action),
                    qUtf8Printable(symbol), conviction, net_qty, edge_dir);
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
    if (action == QLatin1String("status"))
        return ai_handler_status(opts, rest);
    if (action == QLatin1String("run"))
        return ai_handler_run(opts, std::move(rest));
    return ai_handler_usage();
}

}  // namespace openmarketterminal::cli
