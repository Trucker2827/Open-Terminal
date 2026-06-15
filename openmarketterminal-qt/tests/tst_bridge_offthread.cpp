// tst_bridge_offthread.cpp — proves TerminalMcpBridge::handle_post_tool runs
// the internal tool handler OFF the event-loop (main) thread, and that the
// CallFlagGuard's thread-locals are set on that worker thread (so the
// McpProvider auth checker, which reads them during dispatch, sees the right
// value).
//
// Driving model: a blocking BridgeClient call on the MAIN thread would deadlock
// the bridge (its QTcpServer + QFutureWatcher live on the main event loop, which
// the blocking socket wait would starve). So every client request runs on a
// worker thread (QtConcurrent::run on the GLOBAL pool — distinct from the
// bridge's own single-worker tool_pool_) while the main thread pumps the event
// loop via QTRY_VERIFY_WITH_TIMEOUT.

#include <QtTest>
#include <QThread>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>

#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "cli/BridgeDiscoveryFile.h"
#include "cli/BridgeClient.h"

#include <atomic>

using namespace openmarketterminal;

namespace {
// Raw HTTP POST /tool that DOES send X-MCP-Allow-Destructive. BridgeClient
// can't set that header, so we hand-roll the request for the allow-path check.
// Returns the tool result's `success` flag (false on any transport failure).
bool raw_destructive_call(const QByteArray& endpoint, const QByteArray& token,
                          const QByteArray& dtoken, const QByteArray& tool) {
    const QUrl url(QString::fromUtf8(endpoint));
    QTcpSocket sock;
    sock.connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    if (!sock.waitForConnected(3000))
        return false;
    const QByteArray body = "{\"tool\":\"" + tool + "\",\"args\":{}}";
    QByteArray req = "POST /tool HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "X-MCP-Token: " + token + "\r\n";
    req += "X-MCP-Allow-Destructive: " + dtoken + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    req += "\r\n";
    req += body;
    sock.write(req);
    if (!sock.waitForBytesWritten(3000))
        return false;
    QByteArray resp;
    while (sock.state() == QAbstractSocket::ConnectedState && sock.waitForReadyRead(5000))
        resp += sock.readAll();
    resp += sock.readAll();
    const int hdr_end = resp.indexOf("\r\n\r\n");
    if (hdr_end < 0)
        return false;
    const QByteArray payload = resp.mid(hdr_end + 4);
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    return doc.isObject() && doc.object().value("success").toBool();
}
} // namespace

class TstBridgeOffthread : public QObject {
    Q_OBJECT
    static inline std::atomic<Qt::HANDLE> probe_thread{nullptr};
    static inline std::atomic<Qt::HANDLE> auth_thread{nullptr};
    QTemporaryDir home_;

  private slots:
    void initTestCase() {
        // Hermetic: redirect HOME so start()'s bridge.json write lands in a
        // throwaway profile root rather than clobbering a real one.
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
    }

    void tool_runs_off_the_main_thread() {
        mcp::ToolDef probe;
        probe.name = "tst_probe_thread";
        probe.category = "system";
        probe.handler = [](const QJsonObject&) -> mcp::ToolResult {
            probe_thread = QThread::currentThreadId();
            return mcp::ToolResult::ok("ok");
        };
        mcp::McpProvider::instance().register_tools({probe});

        auto& bridge = mcp::TerminalMcpBridge::instance();
        QVERIFY(bridge.start());
        cli::BridgeInfo info;
        info.endpoint = bridge.endpoint();
        info.token = bridge.token();

        // Client runs off-thread; main thread pumps the bridge's event loop.
        auto call = QtConcurrent::run([info]() {
            cli::BridgeClient client(info);
            return client.call_tool("tst_probe_thread", {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(call.isFinished(), 15000);
        const cli::ClientResult r = call.result();

        QCOMPARE(r.status, cli::ClientStatus::Ok);
        QVERIFY(r.body.value("success").toBool());
        QVERIFY(probe_thread.load() != nullptr);
        QVERIFY2(probe_thread.load() != QThread::currentThreadId(),
                 "tool ran on the main/test thread — off-thread dispatch broken");

        bridge.stop();
        mcp::McpProvider::instance().unregister_tool("tst_probe_thread");
    }

    void guard_is_consulted_on_the_worker() {
        mcp::ToolDef probe;
        probe.name = "tst_probe_destructive";
        probe.category = "system";
        probe.is_destructive = true;
        probe.handler = [](const QJsonObject&) -> mcp::ToolResult {
            return mcp::ToolResult::ok("ran");
        };
        mcp::McpProvider::instance().register_tools({probe});

        // Auth checker mimics AgentService: permit a destructive call only when
        // the in-flight bridge call presented the matching destructive token.
        // It reads is_destructive_allowed() — a thread-local set by the
        // CallFlagGuard — and records the thread it ran on.
        mcp::McpProvider::instance().set_auth_checker(
            [](const QString&, const QJsonObject&, mcp::AuthLevel, bool) {
                auth_thread = QThread::currentThreadId();
                return mcp::TerminalMcpBridge::is_destructive_allowed();
            });

        auto& bridge = mcp::TerminalMcpBridge::instance();
        QVERIFY(bridge.start());
        cli::BridgeInfo info;
        info.endpoint = bridge.endpoint();
        info.token = bridge.token();

        // (a) Deny path — no destructive header → guard sets allowed=false on
        //     the worker → checker denies. Round-trip still completes (HTTP 200,
        //     tool success=false).
        auto deny = QtConcurrent::run([info]() {
            cli::BridgeClient client(info);
            return client.call_tool("tst_probe_destructive", {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(deny.isFinished(), 15000);
        const cli::ClientResult dr = deny.result();
        QCOMPARE(dr.status, cli::ClientStatus::Ok);
        QVERIFY2(!dr.body.value("success").toBool(),
                 "destructive tool ran without the header — guard not consulted");
        QVERIFY(auth_thread.load() != nullptr);
        QVERIFY2(auth_thread.load() != QThread::currentThreadId(),
                 "auth check ran on the main thread — it must run where the guard is set");

        // (b) Allow path — raw socket sends the matching destructive token.
        //     This can only SUCCEED if the guard's thread-local is set on the
        //     SAME worker thread the auth check runs on. If the guard were left
        //     on the main thread (the bug), the worker's thread-local would be
        //     false and this call would be denied. This is the discriminating
        //     proof that the guard moved onto the worker.
        const QByteArray endpoint = bridge.endpoint().toUtf8();
        const QByteArray tok = bridge.token().toUtf8();
        const QByteArray dtok = bridge.destructive_token().toUtf8();
        QVERIFY(!dtok.isEmpty());
        auto allow = QtConcurrent::run([endpoint, tok, dtok]() -> bool {
            return raw_destructive_call(endpoint, tok, dtok, "tst_probe_destructive");
        });
        QTRY_VERIFY_WITH_TIMEOUT(allow.isFinished(), 15000);
        QVERIFY2(allow.result(),
                 "destructive tool denied WITH the header — guard thread-local not on the worker");

        mcp::McpProvider::instance().set_auth_checker(nullptr);
        bridge.stop();
        mcp::McpProvider::instance().unregister_tool("tst_probe_destructive");
    }
};

QTEST_MAIN(TstBridgeOffthread)
#include "tst_bridge_offthread.moc"
