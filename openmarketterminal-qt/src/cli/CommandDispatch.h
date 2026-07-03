#pragma once
#include <QStringList>

namespace openmarketterminal::cli {

struct GlobalOpts {
    bool json = false;
    bool headless = false; // run commands in-process via HeadlessRuntime (no GUI)
    bool help = false;
    QString profile = "default";
};

// Parse [--json] [--profile X] off the FRONT of args; mutates args to the
// remaining <group> <command> [params]. Returns false on a bad --profile.
bool parse_global_opts(QStringList& args, GlobalOpts& out);

// Entry: returns a process exit code (see spec §4). Prints data to stdout,
// diagnostics to stderr.
int dispatch(QStringList args);

// Release headless resources before QCoreApplication is destroyed.
void shutdown();

} // namespace openmarketterminal::cli
