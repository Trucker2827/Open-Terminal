#include "cli/BridgeDiscovery.h"

namespace openmarketterminal::cli {

std::variant<Discovered, DiscoveryError> resolve(const QString& profile) {
    const QString root = profile_root_for(profile);
    auto info = read_bridge_file(root);
    if (!info)
        return DiscoveryError::NotRunning;
    if (!is_pid_alive(info->pid))
        return DiscoveryError::Stale;
    return Discovered{*info};
}

const char* describe(DiscoveryError e) {
    switch (e) {
        case DiscoveryError::NotRunning: return "No running OpenTerminal instance for this profile. Start the app (or check 'bridge.autostart').";
        case DiscoveryError::Stale:      return "Stale bridge.json (owning process is gone). Restart OpenTerminal.";
    }
    return "Unknown discovery error";
}

} // namespace openmarketterminal::cli
