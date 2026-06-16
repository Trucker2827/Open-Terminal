#include "cli/CommandDispatch.h"
#include "cli/AiRunCommand.h"
#include "cli/ObserveCommand.h"
#include "cli/BridgeDiscovery.h"
#include "cli/BridgeClient.h"
#include "cli/ServeCommand.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <cstdio>
#include <optional>

namespace openmarketterminal::cli {

bool parse_global_opts(QStringList& args, GlobalOpts& out) {
    while (!args.isEmpty() && args.first().startsWith("--")) {
        const QString flag = args.takeFirst();
        if (flag == "--json") { out.json = true; }
        else if (flag == "--headless") { out.headless = true; }
        else if (flag == "--profile") {
            if (args.isEmpty()) return false;
            out.profile = args.takeFirst();
        } else { args.prepend(flag); break; } // unknown global flag → stop
    }
    return true;
}

static int usage() {
    std::fprintf(stderr,
        "openterminalcli [--json] [--headless] [--profile <name>] <group> <command> [args]\n"
        "  status\n  version\n"
        "  observe latest | week | alerts [N]\n"
        "  mcp list | describe <tool> | call <tool> '<json>'\n"
        "  hub topics | peek <topic> | request <topic>\n"
        "  quote <SYM...>\n"
        "  ai run strategy <meanrev|claude> --mode paper [--interval-sec N]\n"
        "                  [--max-iters M] [--duration-sec D] [--symbols A,B,C]\n");
    return 2;
}

// Resolve the running instance into a client, or print + return an exit code.
static std::optional<BridgeClient> make_client(const GlobalOpts& opts, int& exit_code) {
    auto r = resolve(opts.profile);
    if (auto* d = std::get_if<Discovered>(&r)) return BridgeClient(d->info);
    std::fprintf(stderr, "%s (profile '%s')\n",
                 describe(std::get<DiscoveryError>(r)), qUtf8Printable(opts.profile));
    exit_code = 3;
    return std::nullopt;
}

// Map a ClientResult to (printed output + exit code).
static int emit_result(const GlobalOpts& opts, const ClientResult& r) {
    if (r.status == ClientStatus::Unauthorized) { std::fprintf(stderr, "%s\n", qUtf8Printable(r.error)); return 4; }
    if (r.status != ClientStatus::Ok)           { std::fprintf(stderr, "%s\n", qUtf8Printable(r.error)); return 6; }
    if (r.body.contains("success") && !r.body.value("success").toBool()) {
        std::fprintf(stderr, "tool error: %s\n", qUtf8Printable(r.body.value("error").toString("(no message)")));
        return 5;
    }
    std::printf("%s\n", QJsonDocument(r.body).toJson(
        opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
    return 0;
}

// Map an in-process ToolResult (headless) to (printed output + exit code).
// Mirrors emit_result(ClientResult): a failed tool → exit 5; success prints
// the ToolResult JSON (compact with --json, else indented).
static int emit_result(const GlobalOpts& opts, const mcp::ToolResult& r) {
    if (!r.success) {
        std::fprintf(stderr, "tool error: %s\n",
                     qUtf8Printable(r.error.isEmpty() ? QStringLiteral("(no message)") : r.error));
        return 5;
    }
    std::printf("%s\n", QJsonDocument(r.to_json()).toJson(
        opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
    return 0;
}

// Process-lifetime in-process runtime (headless mode). init() is idempotent, so
// it is brought up lazily by the first command that needs the brain.
static openmarketterminal::headless::HeadlessRuntime& headless_runtime() {
    static openmarketterminal::headless::HeadlessRuntime rt;
    return rt;
}

// Ensure the active transport is ready for a command that talks to the tool tree
// or catalog. Headless → init the in-process runtime (idempotent; init failure
// → exit 7). Attach → resolve a BridgeClient into `client` (discovery failure
// → exit 3 via make_client). Returns false with `exit_code` set on failure.
// `version`/`status` never call this, so they stay pure-local.
static bool prepare_transport(const GlobalOpts& opts, std::optional<BridgeClient>& client,
                              int& exit_code) {
    if (opts.headless) {
        auto ir = headless_runtime().init(opts.profile);
        if (!ir.ok) {
            std::fprintf(stderr, "headless init failed: %s\n", qUtf8Printable(ir.error));
            exit_code = 7;
            return false;
        }
        return true;
    }
    client = make_client(opts, exit_code);
    return client.has_value();
}

// Dispatch one tool call over the active transport and emit its result. Both
// modes share the same (tool, args) construction at the call sites; only the
// transport differs here.
static int exec_tool(const GlobalOpts& opts, std::optional<BridgeClient>& client,
                     const QString& tool, const QJsonObject& args) {
    if (opts.headless)
        return emit_result(opts, headless_runtime().call_tool(tool, args));
    return emit_result(opts, client->call_tool(tool, args));
}

// The headless tool catalog, shaped like the bridge's GET /tools response
// ({"tools": [{name, description, inputSchema, serverId}, ...]}) so `mcp list`
// and `mcp describe` render consistently across modes. Reads the registered
// catalog directly from McpProvider (the full enabled set). Deliberately NOT
// McpService::get_all_tools: that path applies the LLM prompt-size cap
// (kHardMaxTools, sorted alphabetically) which truncates the catalog and drops
// tools like get_quote — wrong for a CLI listing. Requires register_core_tools()
// to have run (prepare_transport ensures it).
static QJsonObject headless_tool_catalog() {
    const auto tools = mcp::McpProvider::instance().list_tools();
    QJsonArray arr;
    for (const auto& t : tools) {
        QJsonObject schema = t.input_schema;
        if (schema.isEmpty()) {
            schema["type"] = "object";
            schema["properties"] = QJsonObject();
        }
        arr.append(QJsonObject{{"name", t.name},
                               {"description", t.description},
                               {"inputSchema", schema},
                               {"serverId", t.server_id}});
    }
    return QJsonObject{{"tools", arr}};
}

int dispatch(QStringList args) {
    GlobalOpts opts;
    if (!parse_global_opts(args, opts)) { std::fprintf(stderr, "error: --profile requires a value\n"); return 2; }
    if (args.isEmpty()) return usage();

    const QString group = args.takeFirst();
    if (group == "version") {
        std::printf("openterminalcli %s\n", qUtf8Printable(QCoreApplication::applicationVersion()));
        return 0;
    }
    if (group == "status") {
        if (opts.headless) {
            // No bridge endpoint in-process — report the in-process mode.
            if (opts.json) {
                QJsonObject o{{"attached", false}, {"mode", "headless"}, {"endpoint", QJsonValue::Null}};
                std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
            } else {
                std::printf("headless (in-process)\n");
            }
            return 0;
        }
        auto r = resolve(opts.profile);
        if (auto* d = std::get_if<Discovered>(&r)) {
            if (opts.json) {
                QJsonObject o{{"attached", true}, {"endpoint", d->info.endpoint}, {"pid", d->info.pid}};
                std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
            } else {
                std::printf("attached  endpoint=%s  pid=%lld\n",
                            qUtf8Printable(d->info.endpoint), static_cast<long long>(d->info.pid));
            }
            return 0;
        }
        std::fprintf(stderr, "%s (profile '%s')\n",
                     describe(std::get<DiscoveryError>(r)), qUtf8Printable(opts.profile));
        return 3;
    }
    if (group == "observe") {
        // Pure-local read of the headless observer's journal (no transport).
        return observe_command(opts, args);
    }
    if (group == "serve") {
        const QString sub = args.isEmpty() ? QString() : args.first();
        if (sub == "--status") return serve_status(opts.profile, opts.json);
        if (sub == "--stop")   return serve_stop(opts.profile);
        if (!args.isEmpty()) { std::fprintf(stderr, "usage: serve [--status|--stop]\n"); return 2; }
        return serve_run(opts.profile);   // blocks in the event loop
    }
    if (group == "mcp") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        if (sub.isEmpty()) { std::fprintf(stderr, "usage: mcp list|describe|call\n"); return 2; }

        std::optional<BridgeClient> c;
        int code = 0;
        if (!prepare_transport(opts, c, code)) return code;

        if (sub == "list") {
            if (opts.headless) {
                std::printf("%s\n", QJsonDocument(headless_tool_catalog()).toJson(
                    opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
                return 0;
            }
            return emit_result(opts, c->get_tools());
        }
        if (sub == "describe") {
            if (args.isEmpty()) { std::fprintf(stderr, "usage: mcp describe <tool>\n"); return 2; }
            const QString want = args.first();
            const QJsonArray tools = opts.headless
                ? headless_tool_catalog().value("tools").toArray()
                : QJsonArray();
            if (!opts.headless) {
                auto r = c->get_tools();
                if (r.status != ClientStatus::Ok) return emit_result(opts, r);
                for (const auto& v : r.body.value("tools").toArray()) {
                    if (v.toObject().value("name").toString() == want) {
                        std::printf("%s\n", QJsonDocument(v.toObject()).toJson(
                            opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
                        return 0;
                    }
                }
                std::fprintf(stderr, "no such tool: %s\n", qUtf8Printable(want)); return 2;
            }
            for (const auto& v : tools) {
                if (v.toObject().value("name").toString() == want) {
                    std::printf("%s\n", QJsonDocument(v.toObject()).toJson(
                        opts.json ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
                    return 0;
                }
            }
            std::fprintf(stderr, "no such tool: %s\n", qUtf8Printable(want)); return 2;
        }
        if (sub == "call") {
            if (args.size() < 1) { std::fprintf(stderr, "usage: mcp call <tool> '<json>'\n"); return 2; }
            const QString tool = args.takeFirst();
            QJsonObject a;
            if (!args.isEmpty()) {
                const QJsonDocument d = QJsonDocument::fromJson(args.first().toUtf8());
                if (!d.isObject()) { std::fprintf(stderr, "args must be a JSON object\n"); return 2; }
                a = d.object();
            }
            return exec_tool(opts, c, tool, a);
        }
        std::fprintf(stderr, "usage: mcp list|describe|call\n"); return 2;
    }
    if (group == "quote") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: quote <SYM...>\n"); return 2; }
        std::optional<BridgeClient> c;
        int code = 0;
        if (!prepare_transport(opts, c, code)) return code;
        int rc = 0;
        for (const QString& sym : args) {
            const int e = exec_tool(opts, c, "get_quote", QJsonObject{{"symbol", sym}});
            if (e != 0) rc = e;
        }
        return rc;
    }

    if (group == "hub") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        if (sub.isEmpty()) { std::fprintf(stderr, "usage: hub topics|peek|request\n"); return 2; }

        QString tool;
        QJsonObject tool_args;
        if (sub == "topics") {
            tool = "datahub_list_topics";
        } else if (sub == "peek" || sub == "request") {
            if (args.isEmpty()) { std::fprintf(stderr, "usage: hub %s <topic>\n", qUtf8Printable(sub)); return 2; }
            tool = (sub == "peek") ? "datahub_peek" : "datahub_request";
            tool_args = QJsonObject{{"topic", args.first()}};
        } else {
            std::fprintf(stderr, "usage: hub topics|peek|request\n"); return 2;
        }

        std::optional<BridgeClient> c;
        int code = 0;
        if (!prepare_transport(opts, c, code)) return code;
        return exec_tool(opts, c, tool, tool_args);
    }

    if (group == "ai") {
        // Only `ai run strategy <name> …` is wired (paper strategy loop).
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        const QString what = args.isEmpty() ? QString() : args.takeFirst();
        if (sub == "run" && what == "strategy")
            return ai_run_strategy(opts, args);  // args == tokens after "ai run strategy"
        std::fprintf(stderr,
                     "usage: ai run strategy <meanrev|claude> --mode paper "
                     "[--interval-sec N] [--max-iters M] [--duration-sec D] [--symbols A,B,C]\n");
        return 2;
    }

    std::fprintf(stderr, "error: unknown command '%s'\n", qUtf8Printable(group));
    return 2;
}

} // namespace openmarketterminal::cli
