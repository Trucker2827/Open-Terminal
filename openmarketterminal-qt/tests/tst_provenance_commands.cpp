#include "cli/CommandDispatch.h"
#include "cli/ProvenanceCommands.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal::cli;

class ProvenanceCommandsTest final : public QObject {
    Q_OBJECT

  private slots:
    void status_is_read_only_when_the_index_is_absent();
    void fixture_verification_does_not_publish();
};

void ProvenanceCommandsTest::status_is_read_only_when_the_index_is_absent() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    GlobalOpts opts;
    opts.json = true;

    QCOMPARE(provenance_command(opts, {QStringLiteral("status"), QStringLiteral("--path"), path}), 0);
    QVERIFY2(!QFileInfo::exists(path), "a read-only status command created the index");
}

void ProvenanceCommandsTest::fixture_verification_does_not_publish() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    const QString fixture = QFINDTESTDATA("fixtures/trade_provenance_foundation_v1.json");
    QVERIFY(!fixture.isEmpty());
    GlobalOpts opts;
    opts.json = true;

    QCOMPARE(provenance_command(opts, {QStringLiteral("verify-rebuild"), QStringLiteral("--fixture"), fixture,
                                       QStringLiteral("--path"), path}),
             4);
    QVERIFY2(!QFileInfo::exists(path), "fixture verification published into the target index");
}

QTEST_MAIN(ProvenanceCommandsTest)
#include "tst_provenance_commands.moc"
