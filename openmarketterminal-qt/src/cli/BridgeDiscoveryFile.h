#pragma once
#include <QString>
#include <optional>

namespace openmarketterminal::cli {

struct BridgeInfo {
    QString endpoint;          // "http://127.0.0.1:<port>"
    QString token;             // X-MCP-Token value
    qint64  pid = 0;           // PID of the GUI that owns the bridge
    QString started_at;        // ISO-8601 UTC
    QString kind;              // "gui" | "daemon"
    // NOTE: bridge.json carries NO destructive token. In attach mode the CLI is
    // read + non-destructive: it has no consumer for a destructive token and
    // never sends X-MCP-Allow-Destructive. CLI destructive/trading is a future
    // phase. (The in-process destructive_token_ used by in-app Python agents
    // lives only in TerminalMcpBridge and is never written to disk.)
};

/// Replicates AppPaths::root()+ProfileManager::profile_root() WITHOUT linking
/// the GUI. Must stay byte-identical to those functions.
QString profile_root_for(const QString& profile);

/// "<profile_root>/bridge.json"
QString bridge_file_path(const QString& profile_root);

/// Write bridge.json (pretty JSON) with 0600 perms. Returns false on failure.
bool write_bridge_file(const QString& profile_root, const BridgeInfo& info);

/// Remove bridge.json if present. Returns true if absent or successfully removed.
bool remove_bridge_file(const QString& profile_root);

/// Parse bridge.json. Returns nullopt if missing, unreadable, or malformed.
std::optional<BridgeInfo> read_bridge_file(const QString& profile_root);

/// True if a process with this PID currently exists (kill(pid,0) / Win OpenProcess).
bool is_pid_alive(qint64 pid);

} // namespace openmarketterminal::cli
