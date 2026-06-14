#include "cli/CommandDispatch.h"
#include "cli/BridgeDiscovery.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <cstdio>

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
        "  hub topics | peek <topic> | request <topic> [k=v...]\n"
        "  quote <SYM...>\n");
    return 2;
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
        std::fprintf(stderr, "%s\n", describe(std::get<DiscoveryError>(r)));
        return 3;
    }
    // mcp / hub / quote wired in Tasks 7,8.
    std::fprintf(stderr, "error: unknown command '%s'\n", qUtf8Printable(group));
    return 2;
}

} // namespace openmarketterminal::cli
