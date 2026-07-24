#include "cli/BridgeClient.h"
#include <QElapsedTimer>
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

    // Connection: close framing — the response is complete when the server
    // closes the socket, not when the line goes quiet. An idle-window read here
    // truncates slow tools (a Quant Lab call legitimately computes for >5s
    // before its first byte), so wait until close, bounded only by the total
    // deadline.
    QByteArray resp;
    QElapsedTimer waited;
    waited.start();
    while (sock.state() == QAbstractSocket::ConnectedState) {
        const qint64 remaining = total_timeout_ms_ - waited.elapsed();
        if (remaining <= 0)
            break;
        sock.waitForReadyRead(static_cast<int>(qMin<qint64>(remaining, 1000)));
        resp += sock.readAll();
    }
    resp += sock.readAll();

    const int hdr_end = resp.indexOf("\r\n\r\n");
    if (hdr_end < 0)
        return {ClientStatus::BadResponse, {},
                QString("no header terminator (%1 bytes after %2ms)").arg(resp.size()).arg(waited.elapsed())};
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
