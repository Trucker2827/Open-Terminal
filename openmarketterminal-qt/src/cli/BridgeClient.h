#pragma once
#include "cli/BridgeDiscoveryFile.h"
#include <QJsonObject>
#include <QJsonArray>

namespace openmarketterminal::cli {

enum class ClientStatus { Ok, Unauthorized, Transport, BadResponse };

struct ClientResult {
    ClientStatus status = ClientStatus::Transport;
    QJsonObject body;     // parsed JSON (tool result or {tools:[...]})
    QString error;        // human message when !Ok
};

class BridgeClient {
public:
    // total_timeout_ms bounds the whole response wait. Tool calls run
    // synchronously in the bridge — Quant Lab tools spawn a Python subprocess
    // whose cold imports (torch, statsmodels) alone can take tens of seconds
    // before the first response byte, so this must be generous.
    explicit BridgeClient(BridgeInfo info, int total_timeout_ms = 180000)
        : info_(std::move(info)), total_timeout_ms_(total_timeout_ms) {}

    ClientResult get_tools();                                   // GET /tools
    ClientResult call_tool(const QString& name, const QJsonObject& args); // POST /tool

private:
    ClientResult request(const QByteArray& method, const QString& path, const QByteArray& body);
    BridgeInfo info_;
    int total_timeout_ms_;
};

} // namespace openmarketterminal::cli
