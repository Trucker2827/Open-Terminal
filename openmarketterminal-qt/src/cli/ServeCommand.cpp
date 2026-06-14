#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include <QCoreApplication>
#include <QSocketNotifier>
#include <cstdio>
#include <csignal>
#include <optional>
#ifndef _WIN32
#  include <unistd.h>
#endif

namespace openmarketterminal::cli {
namespace {
#ifndef _WIN32
int g_sigfd[2] = {-1, -1};                       // self-pipe: handler writes, notifier reads
void on_signal(int) { char c = 1; ssize_t n = ::write(g_sigfd[1], &c, 1); (void)n; }
#endif
} // namespace

int serve_run(const QString& profile) {
    const QString root = profile_root_for(profile);
    // Single-owner: refuse if a live instance (GUI or daemon) already owns it.
    if (auto info = read_bridge_file(root); info && is_pid_alive(info->pid)) {
        std::fprintf(stderr, "An instance already owns profile '%s' (%s, pid %lld) at %s\n",
                     qUtf8Printable(profile), qUtf8Printable(info->kind),
                     static_cast<long long>(info->pid), qUtf8Printable(info->endpoint));
        return 3;
    }

    headless::HeadlessRuntime rt;
    if (auto r = rt.init(profile); !r.ok) {
        std::fprintf(stderr, "daemon init failed: %s\n", qUtf8Printable(r.error));
        return 7;
    }

    // Read-only over the bridge: deny destructive AND settings-write regardless
    // of toggles (writes/destructive over a long-lived daemon need the revocable
    // -token design — deferred). Reads + the >=Verified floor unchanged.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject&, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (is_destructive) return false;            // daemon MVP: no writes/destructive
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });

    auto& bridge = mcp::TerminalMcpBridge::instance();
    bridge.set_owner_kind("daemon");
    if (!bridge.start()) {                                 // binds 127.0.0.1 + writes bridge.json(kind=daemon)
        std::fprintf(stderr, "daemon: failed to start the bridge\n");
        return 7;
    }
    std::fprintf(stderr, "openterminalcli serve: %s (profile '%s', pid %lld). Ctrl-C / SIGTERM to stop.\n",
                 qUtf8Printable(bridge.endpoint()), qUtf8Printable(profile),
                 static_cast<long long>(QCoreApplication::applicationPid()));

#ifndef _WIN32
    // Clean shutdown on SIGTERM/SIGINT via the self-pipe trick (async-signal-safe).
    if (::pipe(g_sigfd) == 0) {
        auto* sn = new QSocketNotifier(g_sigfd[0], QSocketNotifier::Read, qApp);
        QObject::connect(sn, &QSocketNotifier::activated, qApp, []() { QCoreApplication::quit(); });
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
    }
#endif

    const int rc = QCoreApplication::exec();              // feeds/subscriptions live here
    bridge.stop();                                        // removes bridge.json
    return rc;
}

// status()/stop() are implemented in Task 3; stubbed here so the dispatch routes
// link. (Returning 2 = usage/not-implemented.)
int serve_status(const QString&, bool) { return 2; }
int serve_stop(const QString&) { return 2; }

} // namespace openmarketterminal::cli
