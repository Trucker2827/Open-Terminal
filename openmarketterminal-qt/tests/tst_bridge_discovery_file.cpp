#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "cli/BridgeDiscoveryFile.h"

using namespace openmarketterminal::cli;

class TstBridgeDiscoveryFile : public QObject {
    Q_OBJECT
private slots:
    void profile_root_default_matches_literal() {
        const QString r = profile_root_for("default");
        QVERIFY(r.endsWith("org.openterminal.OpenTerminal"));
        QVERIFY(!r.contains("/profiles/"));
    }
    void profile_root_named_appends_profiles() {
        const QString r = profile_root_for("alice");
        QVERIFY(r.endsWith("org.openterminal.OpenTerminal/profiles/alice"));
    }
    void write_then_read_roundtrips() {
        QTemporaryDir dir;
        BridgeInfo in{"http://127.0.0.1:54923", "tok-123", 4242, "2026-06-14T00:00:00Z"};
        QVERIFY(write_bridge_file(dir.path(), in));
        auto out = read_bridge_file(dir.path());
        QVERIFY(out.has_value());
        QCOMPARE(out->endpoint, in.endpoint);
        QCOMPARE(out->token, in.token);
        QCOMPARE(out->pid, in.pid);
        QCOMPARE(out->started_at, in.started_at);
    }
    void file_is_owner_only() {
        QTemporaryDir dir;
        write_bridge_file(dir.path(), {"http://127.0.0.1:1", "t", 1, "x"});
        const auto perms = QFile::permissions(bridge_file_path(dir.path()));
        QVERIFY(!(perms & (QFile::ReadGroup | QFile::ReadOther |
                           QFile::WriteGroup | QFile::WriteOther)));
    }
    void read_missing_is_nullopt() {
        QTemporaryDir dir;
        QVERIFY(!read_bridge_file(dir.path()).has_value());
    }
    void read_missing_required_fields_is_nullopt() {
        QTemporaryDir dir;
        QFile f(bridge_file_path(dir.path()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"schema":1,"pid":1})"); f.close();
        QVERIFY(!read_bridge_file(dir.path()).has_value());
    }
    void read_malformed_is_nullopt() {
        QTemporaryDir dir;
        QFile f(bridge_file_path(dir.path()));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write("not json{"); f.close();
        QVERIFY(!read_bridge_file(dir.path()).has_value());
    }
    void remove_is_idempotent() {
        QTemporaryDir dir;
        QVERIFY(remove_bridge_file(dir.path()));            // absent → true
        write_bridge_file(dir.path(), {"e","t",1,"x"});
        QVERIFY(remove_bridge_file(dir.path()));            // present → removed
        QVERIFY(!QFile::exists(bridge_file_path(dir.path())));
    }
    void self_pid_is_alive() {
        QVERIFY(is_pid_alive(QCoreApplication::applicationPid()));
        QVERIFY(!is_pid_alive(999999999));
    }
};
QTEST_MAIN(TstBridgeDiscoveryFile)
#include "tst_bridge_discovery_file.moc"
