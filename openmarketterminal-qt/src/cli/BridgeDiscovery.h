#pragma once
#include "cli/BridgeDiscoveryFile.h"
#include <QString>
#include <variant>

namespace openmarketterminal::cli {

enum class DiscoveryError { NotRunning, Stale };

struct Discovered { BridgeInfo info; };

// Read bridge.json for `profile`; verify the owning PID is alive.
std::variant<Discovered, DiscoveryError> resolve(const QString& profile);

const char* describe(DiscoveryError e);

} // namespace openmarketterminal::cli
