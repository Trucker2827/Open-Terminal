#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "screens/kalshi/CliLocator.h"

using namespace openmarketterminal::screens::kalshi;

namespace {
// Drops an executable file at path (0755 so the Unix isExecutable probe passes;
// on Windows the ".exe" extension is what makes it executable).
void make_executable(const QString& path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\n");
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
}
}  // namespace

class KalshiCliLocatorTest final : public QObject {
    Q_OBJECT
  private slots:
    void candidates_carry_the_platform_suffix() {
        // POSIX-style absolute path: QDir::cleanPath treats "C:" as a
        // relative segment on non-Windows hosts, and the suffix handling
        // under test is orthogonal to drive letters.
        const QStringList windows = cli_candidate_paths(QStringLiteral("/apps/sub/OT"),
                                                        QStringLiteral(".exe"));
        QCOMPARE(windows.size(), 2);
        QCOMPARE(windows.at(0), QStringLiteral("/apps/sub/OT/openterminalcli.exe"));
        QCOMPARE(windows.at(1), QStringLiteral("/openterminalcli.exe"));
        const QStringList unix = cli_candidate_paths(QStringLiteral("/opt/OT/bin"), QString());
        QCOMPARE(unix.at(0), QStringLiteral("/opt/OT/bin/openterminalcli"));
    }

    void finds_exe_beside_app_with_windows_suffix() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        make_executable(dir.filePath(QStringLiteral("openterminalcli.exe")));
        const QString found = find_cli_beside_app(dir.path(), QStringLiteral(".exe"));
        QCOMPARE(found, dir.filePath(QStringLiteral("openterminalcli.exe")));
    }

    void suffix_mismatch_reads_missing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        make_executable(dir.filePath(QStringLiteral("openterminalcli.exe")));
        // Probing without the suffix must NOT fabricate a hit from the .exe.
        QVERIFY(find_cli_beside_app(dir.path(), QString()).isEmpty());
    }

    void empty_dir_reads_missing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(find_cli_beside_app(dir.path(), QStringLiteral(".exe")).isEmpty());
        QVERIFY(find_cli_beside_app(dir.path(), QString()).isEmpty());
    }

    void probes_three_levels_up_for_bundle_layout() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString app_dir =
            root.filePath(QStringLiteral("OpenMarketTerminal.app/Contents/MacOS"));
        QVERIFY(QDir().mkpath(app_dir));
        make_executable(root.filePath(QStringLiteral("openterminalcli.exe")));
        const QString found = find_cli_beside_app(app_dir, QStringLiteral(".exe"));
        QCOMPARE(found, root.filePath(QStringLiteral("openterminalcli.exe")));
    }

    void platform_suffix_matches_the_host() {
#ifdef Q_OS_WIN
        QCOMPARE(cli_exe_suffix(), QStringLiteral(".exe"));
#else
        QVERIFY(cli_exe_suffix().isEmpty());
#endif
    }
};

QTEST_GUILESS_MAIN(KalshiCliLocatorTest)
#include "tst_kalshi_cli_locator.moc"
