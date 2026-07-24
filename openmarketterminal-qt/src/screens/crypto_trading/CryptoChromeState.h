#pragma once
// Truthful status-chrome state mapping for the crypto command bar.
// Pure string mapping so the tri-state rules are unit-testable.

#include <QString>

namespace openmarketterminal::crypto {

/// API button: "none" (no credentials / never probed), "ok" (last
/// authenticated call succeeded), "error" (credentials present, last call
/// failed). last_auth_state follows the screen convention: -1 unknown,
/// 0 failed, 1 ok.
inline QString chrome_api_state(bool has_credentials, int last_auth_state) {
    if (!has_credentials || last_auth_state < 0)
        return QStringLiteral("none");
    return last_auth_state == 1 ? QStringLiteral("ok") : QStringLiteral("error");
}

/// DAEMON label: "dead" (subprocess not running), "rest" (daemon up, public
/// WS not connected — REST fallback), "live" (daemon up + WS streaming).
inline QString chrome_daemon_state(bool daemon_alive, bool ws_connected) {
    if (!daemon_alive)
        return QStringLiteral("dead");
    return ws_connected ? QStringLiteral("live") : QStringLiteral("rest");
}

} // namespace openmarketterminal::crypto
