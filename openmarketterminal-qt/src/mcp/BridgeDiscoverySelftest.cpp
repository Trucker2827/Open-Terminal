#include "mcp/BridgeDiscoverySelftest.h"
#include "mcp/TerminalMcpBridge.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/config/ProfileManager.h"
#include <cstdio>

namespace openmarketterminal::mcp {

int run_bridge_discovery_selftest() {
    auto& bridge = TerminalMcpBridge::instance();
    const QString root = ProfileManager::instance().profile_root();
    cli::remove_bridge_file(root);

    if (!bridge.start()) { std::fprintf(stderr, "FAIL: bridge.start()\n"); return 1; }
    auto info = cli::read_bridge_file(root);
    if (!info) { std::fprintf(stderr, "FAIL: bridge.json not written\n"); return 1; }
    if (info->endpoint != bridge.endpoint() || info->token != bridge.token()) {
        std::fprintf(stderr, "FAIL: bridge.json mismatch\n"); return 1;
    }
    bridge.stop();
    if (cli::read_bridge_file(root)) { std::fprintf(stderr, "FAIL: bridge.json not removed on stop\n"); return 1; }
    std::printf("OK: bridge discovery write/remove\n");
    return 0;
}

} // namespace openmarketterminal::mcp
