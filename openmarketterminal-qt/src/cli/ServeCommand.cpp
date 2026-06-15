#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include <QCoreApplication>
#include <QSocketNotifier>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <cstdio>
#include <csignal>
#include <optional>
#ifndef _WIN32
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
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
    // NOTE: this gate covers only McpProvider (internal) tool calls. If a future
    // task initializes McpService (external MCP servers) in the daemon, those
    // calls would bypass this checker and need their own read-only gate.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject& args, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (tool == "submit_order") {
                // Normalize identically to the handler so a case/whitespace variant
                // can't take a different branch than the handler will.
                const QString mode = args.value("mode").toString().trimmed().toLower();
                if (mode == "paper") return true;                // reach the handler; it enforces the toggle + executes
                return mcp::cli_trading_allowed() && mcp::cli_live_armed();  // live: false in Phase A
            }
            if (is_destructive) return false;            // daemon MVP: no writes/destructive
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });

#ifndef _WIN32
    // Clean shutdown on SIGTERM/SIGINT via the self-pipe trick (async-signal-safe).
    // Install the handlers BEFORE bridge.start() so a signal arriving during/after
    // start() routes through the notifier (clean stop) rather than killing the
    // process with a stale bridge.json on disk. The pipe + std::signal install do
    // not depend on the bridge; the QSocketNotifier needs qApp, which exists.
    // O_CLOEXEC keeps the self-pipe fds out of any child processes the daemon spawns.
    bool sig_ready = false;
#  if defined(__linux__)
    sig_ready = (::pipe2(g_sigfd, O_CLOEXEC) == 0);       // Linux: atomic CLOEXEC
#  endif
    if (!sig_ready && ::pipe(g_sigfd) == 0) {             // macOS fallback: pipe + fcntl
        ::fcntl(g_sigfd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(g_sigfd[1], F_SETFD, FD_CLOEXEC);
        sig_ready = true;
    }
    if (sig_ready) {
        auto* sn = new QSocketNotifier(g_sigfd[0], QSocketNotifier::Read, qApp);
        QObject::connect(sn, &QSocketNotifier::activated, qApp, []() { QCoreApplication::quit(); });
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
    }
#endif

    auto& bridge = mcp::TerminalMcpBridge::instance();
    bridge.set_owner_kind("daemon");
    if (!bridge.start()) {                                 // binds 127.0.0.1 + writes bridge.json(kind=daemon)
        std::fprintf(stderr, "daemon: failed to start the bridge\n");
        return 7;
    }
    std::fprintf(stderr, "openterminalcli serve: %s (profile '%s', pid %lld). Ctrl-C / SIGTERM to stop.\n",
                 qUtf8Printable(bridge.endpoint()), qUtf8Printable(profile),
                 static_cast<long long>(QCoreApplication::applicationPid()));

    const int rc = QCoreApplication::exec();              // feeds/subscriptions live here
    bridge.stop();                                        // removes bridge.json
    return rc;
}

int serve_status(const QString& profile, bool json) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool live = info && is_pid_alive(info->pid);
    if (json) {
        QJsonObject o{{"running", live}};
        if (live) { o["endpoint"]=info->endpoint; o["pid"]=info->pid; o["kind"]=info->kind; }
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (live) {
        std::printf("running  kind=%s  endpoint=%s  pid=%lld\n",
                    qUtf8Printable(info->kind), qUtf8Printable(info->endpoint),
                    static_cast<long long>(info->pid));
    } else {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
    }
    return live ? 0 : 3;
}

int serve_stop(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    if (!info || !is_pid_alive(info->pid)) {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    if (info->kind != "daemon") {
        std::fprintf(stderr, "owner is a %s, not a daemon — refusing to stop it (quit it directly)\n",
                     qUtf8Printable(info->kind));
        return 3;
    }
#ifndef _WIN32
    if (::kill(static_cast<pid_t>(info->pid), SIGTERM) != 0) {
        std::fprintf(stderr, "failed to signal daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    // Grace: wait up to ~5s for clean exit; escalate to SIGKILL if still alive.
    for (int i = 0; i < 50 && is_pid_alive(info->pid); ++i)
        QThread::msleep(100);
    if (is_pid_alive(info->pid)) {
        std::fprintf(stderr, "daemon pid %lld did not exit on SIGTERM; sending SIGKILL\n",
                     static_cast<long long>(info->pid));
        ::kill(static_cast<pid_t>(info->pid), SIGKILL);
        for (int i = 0; i < 20 && is_pid_alive(info->pid); ++i) QThread::msleep(100);
        // A SIGKILLed daemon can't run its aboutToQuit cleanup, so the stale
        // bridge.json may remain; remove it so the next attach/serve is clean.
        remove_bridge_file(profile_root_for(profile));
        std::printf("daemon pid %lld force-stopped (SIGKILL)\n", static_cast<long long>(info->pid));
        return 0;
    }
    std::printf("sent SIGTERM to daemon pid %lld\n", static_cast<long long>(info->pid));
    return 0;
#endif
}

} // namespace openmarketterminal::cli
