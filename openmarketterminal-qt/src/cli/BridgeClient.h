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
    explicit BridgeClient(BridgeInfo info) : info_(std::move(info)) {}

    ClientResult get_tools();                                   // GET /tools
    ClientResult call_tool(const QString& name, const QJsonObject& args); // POST /tool

private:
    ClientResult request(const QByteArray& method, const QString& path, const QByteArray& body);
    BridgeInfo info_;
};

} // namespace openmarketterminal::cli
