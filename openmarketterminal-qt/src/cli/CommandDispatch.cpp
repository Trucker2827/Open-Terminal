#include "cli/CommandDispatch.h"
#include "cli/BridgeDiscovery.h"
#include "cli/BridgeClient.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <cstdio>
#include <optional>

namespace openmarketterminal::cli {

bool parse_global_opts(QStringList& args, GlobalOpts& out) {
    while (!args.isEmpty() && args.first().startsWith("--")) {
        const QString flag = args.takeFirst();
        if (flag == "--json") { out.json = true; }
        else if (flag == "--profile") {
            if (args.isEmpty()) return false;
            out.profile = args.takeFirst();
        } else { args.prepend(flag); break; } // unknown global flag → stop
    }
    return true;
}

static int usage() {
    std::fprintf(stderr,
        "openterminalcli [--json] [--profile <name>] <group> <command> [args]\n"
        "  status\n  version\n"
        "  mcp list | describe <tool> | call <tool> '<json>'\n"
        "  hub topics | peek <topic> | request <topic>\n"
        "  quote <SYM...>\n");
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
    if (group == "mcp") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        if (sub.isEmpty()) { std::fprintf(stderr, "usage: mcp list|describe|call\n"); return 2; }
        int code = 0;
        auto c = make_client(opts, code);
        if (!c) return code;

        if (sub == "list") {
            auto r = c->get_tools(); return emit_result(opts, r);
        }
        if (sub == "describe") {
            if (args.isEmpty()) { std::fprintf(stderr, "usage: mcp describe <tool>\n"); return 2; }
            const QString want = args.first();
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
        if (sub == "call") {
            if (args.size() < 1) { std::fprintf(stderr, "usage: mcp call <tool> '<json>'\n"); return 2; }
            const QString tool = args.takeFirst();
            QJsonObject a;
            if (!args.isEmpty()) {
                const QJsonDocument d = QJsonDocument::fromJson(args.first().toUtf8());
                if (!d.isObject()) { std::fprintf(stderr, "args must be a JSON object\n"); return 2; }
                a = d.object();
            }
            auto r = c->call_tool(tool, a); return emit_result(opts, r);
        }
        std::fprintf(stderr, "usage: mcp list|describe|call\n"); return 2;
    }
    if (group == "quote") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: quote <SYM...>\n"); return 2; }
        int code = 0;
        auto c = make_client(opts, code);
        if (!c) return code;
        int rc = 0;
        for (const QString& sym : args) {
            auto r = c->call_tool("get_quote", QJsonObject{{"symbol", sym}});
            const int e = emit_result(opts, r);
            if (e != 0) rc = e;
        }
        return rc;
    }

    if (group == "hub") {
        const QString sub = args.isEmpty() ? QString() : args.takeFirst();
        if (sub.isEmpty()) { std::fprintf(stderr, "usage: hub topics|peek|request\n"); return 2; }
        int code = 0;
        auto c = make_client(opts, code);
        if (!c) return code;
        ClientResult r;
        if (sub == "topics") {
            r = c->call_tool("datahub_list_topics", {});
        } else if (sub == "peek" || sub == "request") {
            if (args.isEmpty()) { std::fprintf(stderr, "usage: hub %s <topic>\n", qUtf8Printable(sub)); return 2; }
            const QString tool = (sub == "peek") ? "datahub_peek" : "datahub_request";
            r = c->call_tool(tool, QJsonObject{{"topic", args.first()}});
        } else {
            std::fprintf(stderr, "usage: hub topics|peek|request\n"); return 2;
        }
        return emit_result(opts, r);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n", qUtf8Printable(group));
    return 2;
}

} // namespace openmarketterminal::cli
