#include "mcp/BridgeDiscoverySelftest.h"
#include "mcp/TerminalMcpBridge.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/config/ProfileManager.h"
#include <QFile>
#include <cstdio>

namespace openmarketterminal::mcp {

int run_bridge_discovery_selftest() {
    auto& bridge = TerminalMcpBridge::instance();
    const QString root = ProfileManager::instance().profile_root();
    // This selftest links both worlds, so assert the CLI's standalone path replica
    // (cli::profile_root_for) still matches the GUI's real ProfileManager path.
    const QString expect = ProfileManager::instance().profile_root();
    if (cli::profile_root_for(ProfileManager::instance().active()) != expect) {
        std::fprintf(stderr, "FAIL: profile_root_for diverged from ProfileManager::profile_root()\n");
        return 1;
    }
    cli::remove_bridge_file(root);

    if (!bridge.start()) { std::fprintf(stderr, "FAIL: bridge.start()\n"); return 1; }
    auto info = cli::read_bridge_file(root);
    if (!info) { std::fprintf(stderr, "FAIL: bridge.json not written\n"); return 1; }
    if (info->endpoint != bridge.endpoint() || info->token != bridge.token()) {
        std::fprintf(stderr, "FAIL: bridge.json mismatch\n"); return 1;
    }
    // Security: bridge.json must carry NO destructive token (raw-byte check so a
    // re-added writer fails red even though read_bridge_file no longer parses it).
    {
        QFile f(cli::bridge_file_path(root));
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: cannot reopen bridge.json\n"); return 1;
        }
        if (f.readAll().contains("destructive_token")) {
            std::fprintf(stderr, "FAIL: bridge.json must not contain destructive_token\n");
            return 1;
        }
    }
    bridge.stop();
    if (cli::read_bridge_file(root)) { std::fprintf(stderr, "FAIL: bridge.json not removed on stop\n"); return 1; }
    std::printf("OK: bridge discovery write/remove\n");
    return 0;
}

} // namespace openmarketterminal::mcp
