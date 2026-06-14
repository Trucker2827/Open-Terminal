#include "cli/CommandDispatch.h"
#include <QCoreApplication>
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
    // status / mcp / hub / quote wired in Tasks 5,7,8.
    std::fprintf(stderr, "error: unknown command '%s'\n", qUtf8Printable(group));
    return 2;
}

} // namespace openmarketterminal::cli
