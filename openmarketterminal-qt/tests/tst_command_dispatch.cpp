#include <QtTest>
#include <QTemporaryDir>
#include "cli/CommandDispatch.h"
#include "cli/BridgeDiscovery.h"
using namespace openmarketterminal::cli;

class TstCommandDispatch : public QObject {
    Q_OBJECT
private slots:
    void strips_global_flags() {
        QStringList args{"--json", "--profile", "alice", "mcp", "list"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(o.json);
        QCOMPARE(o.profile, QString("alice"));
        QCOMPARE(args, (QStringList{"mcp", "list"}));
    }
    void defaults_when_absent() {
        QStringList args{"status"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(!o.json);
        QCOMPARE(o.profile, QString("default"));
        QCOMPARE(args, (QStringList{"status"}));
    }
    void rejects_dangling_profile() {
        QStringList args{"--profile"};
        GlobalOpts o;
        QVERIFY(!parse_global_opts(args, o));
    }
    void resolve_missing_is_not_running() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<DiscoveryError>(r));
        QCOMPARE(std::get<DiscoveryError>(r), DiscoveryError::NotRunning);
    }
    void resolve_live_pid_succeeds() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        write_bridge_file(profile_root_for("default"),
            {"http://127.0.0.1:9", "tok", QCoreApplication::applicationPid(), "x"});
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<Discovered>(r));
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
