#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QSemaphore>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include "cli/BridgeClient.h"
using namespace openmarketterminal::cli;

// Minimal fake of TerminalMcpBridge: checks X-MCP-Token, answers /tools and /tool.
//
// NOTE (test-harness deviation from the plan): the synchronous BridgeClient uses
// QTcpSocket::waitFor* which blocks on a single fd and does NOT pump the Qt event
// loop. If the fake server lived on the test thread, its newConnection/readyRead
// slots (event-loop-driven) would never fire while the client blocks → deadlock
// (each call times out at 5s). In production the bridge is a separate process, so
// the synchronous client is correct. Here we run the fake on its own thread with
// its own event loop. The request-handler body is identical to the plan's.
class FakeBridge : public QObject {
    Q_OBJECT
public:
    quint16 port = 0;
    QString token = "secret";
    FakeBridge() {
        thread_ = QThread::create([this] {
            QTcpServer srv;
            srv.listen(QHostAddress::LocalHost, 0);
            port = srv.serverPort();
            portReady_.release();
            QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this, &srv] {
                QTcpSocket* s = srv.nextPendingConnection();
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const QByteArray req = s->readAll();
                    // Safety invariant: the read-only CLI must never offer the
                    // destructive-action token over the wire.
                    QVERIFY(!req.contains("X-MCP-Allow-Destructive"));
                    const bool authed = req.contains("X-MCP-Token: " + token.toUtf8());
                    QByteArray body; int code = 200;
                    if (!authed) { code = 401; body = R"({"success":false,"error":"bad token"})"; }
                    else if (req.startsWith("GET /tools")) body = R"({"tools":[{"name":"get_quote"}]})";
                    else if (req.startsWith("POST /tool")) {
                        if (req.contains("\"fail\"")) {
                            body = R"({"success":false,"error":"nope"})";
                        } else {
                            const int b = req.indexOf("\r\n\r\n");
                            const QByteArray sent = req.mid(b + 4);
                            body = "{\"success\":true,\"echo\":" + sent + "}";
                        }
                    }
                    QByteArray resp = "HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + body;
                    s->write(resp); s->flush(); s->disconnectFromHost();
                });
            });
            QEventLoop loop;
            QTimer t;
            QObject::connect(&t, &QTimer::timeout, &loop, [this, &loop] {
                if (stop_.load()) loop.quit();
            });
            t.start(20);
            loop.exec();
        });
        thread_->start();
        portReady_.acquire();
    }
    ~FakeBridge() override {
        stop_.store(true);
        thread_->wait();
        delete thread_;
    }
private:
    QThread* thread_ = nullptr;
    QSemaphore portReady_;
    std::atomic<bool> stop_{false};
};

class TstBridgeClient : public QObject {
    Q_OBJECT
private slots:
    void get_tools_ok() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.get_tools();
        QCOMPARE(r.status, ClientStatus::Ok);
        QVERIFY(r.body.value("tools").toArray().size() == 1);
    }
    void bad_token_is_unauthorized() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "WRONG", 0, ""});
        QCOMPARE(c.get_tools().status, ClientStatus::Unauthorized);
    }
    void call_tool_failure_parses() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.call_tool("x", QJsonObject{{"fail", true}});
        QCOMPARE(r.status, ClientStatus::Ok);                 // transport ok…
        QCOMPARE(r.body.value("success").toBool(), false);    // …tool reported failure
    }
    void dead_endpoint_is_transport() {
        BridgeClient c({"http://127.0.0.1:1", "secret", 0, ""});
        QCOMPARE(c.get_tools().status, ClientStatus::Transport);
    }
    void call_tool_sends_name_and_args() {
        FakeBridge fb;
        BridgeClient c({QString("http://127.0.0.1:%1").arg(fb.port), "secret", 0, ""});
        auto r = c.call_tool("get_quote", QJsonObject{{"symbol","AAPL"}});
        QCOMPARE(r.status, ClientStatus::Ok);
        const QJsonObject echo = r.body.value("echo").toObject();
        QCOMPARE(echo.value("tool").toString(), QString("get_quote"));
        QCOMPARE(echo.value("args").toObject().value("symbol").toString(), QString("AAPL"));
    }
};
QTEST_MAIN(TstBridgeClient)
#include "tst_bridge_client.moc"
