// QuantLibLocalServer.h — Launches + manages the bundled local QuantLib REST
// server (scripts/quantlib/quantlib_server.py) so the QuantLib screen can
// actually compute against real QuantLib results.

#pragma once

#include <QProcess>

namespace openmarketterminal::services {

// Singleton that auto-starts the local QuantLib pricing server and points the
// app's connectors.quantlib_url setting at it. The owned QProcess is terminated
// on application quit so we never orphan a server.
class QuantLibLocalServer {
public:
    static QuantLibLocalServer& instance();

    // Idempotent: starts the server once. Safe to call from every screen ctor.
    void ensure_running();

private:
    QuantLibLocalServer() = default;
    ~QuantLibLocalServer() = default;
    QuantLibLocalServer(const QuantLibLocalServer&) = delete;
    QuantLibLocalServer& operator=(const QuantLibLocalServer&) = delete;

    bool started_ = false;
    QProcess* proc_ = nullptr;
};

} // namespace openmarketterminal::services
