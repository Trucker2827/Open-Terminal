#include <QtTest>
#include "cli/CommandDispatch.h"
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
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
