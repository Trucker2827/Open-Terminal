#include "cli/BridgeClient.h"
#include <QJsonDocument>
#include <QTcpSocket>
#include <QUrl>

namespace openmarketterminal::cli {

ClientResult BridgeClient::request(const QByteArray& method, const QString& path, const QByteArray& body) {
    const QUrl url(info_.endpoint);
    QTcpSocket sock;
    sock.connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    if (!sock.waitForConnected(3000))
        return {ClientStatus::Transport, {}, "cannot connect to " + info_.endpoint};

    QByteArray req = method + " " + path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "X-MCP-Token: " + info_.token.toUtf8() + "\r\n";
    req += "Connection: close\r\n";
    if (!body.isEmpty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    sock.write(req);
    if (!sock.waitForBytesWritten(3000))
        return {ClientStatus::Transport, {}, "write failed"};

    QByteArray resp;
    while (sock.state() == QAbstractSocket::ConnectedState && sock.waitForReadyRead(5000))
        resp += sock.readAll();
    resp += sock.readAll();

    const int hdr_end = resp.indexOf("\r\n\r\n");
    if (hdr_end < 0)
        return {ClientStatus::BadResponse, {}, "no header terminator"};
    const QByteArray status_line = resp.left(resp.indexOf("\r\n"));
    const QByteArray payload = resp.mid(hdr_end + 4);
    const int code = status_line.split(' ').value(1).toInt();

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};

    if (code == 401)
        return {ClientStatus::Unauthorized, obj, "bridge rejected token (restart? re-read bridge.json)"};
    if (code != 200)
        return {ClientStatus::BadResponse, obj, "HTTP " + QString::number(code)};
    if (!doc.isObject())
        return {ClientStatus::BadResponse, {}, "non-JSON response"};
    return {ClientStatus::Ok, obj, {}};
}

ClientResult BridgeClient::get_tools() {
    // ?all=1 → full uncapped catalogue. The CLI / Claude Code MCP adapter does its
    // own tool selection, so it wants every tool, not the token-limited agent subset.
    return request("GET", "/tools?all=1", {});
}

ClientResult BridgeClient::call_tool(const QString& name, const QJsonObject& args) {
    QJsonObject body{{"tool", name}, {"args", args}};
    return request("POST", "/tool", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace openmarketterminal::cli
