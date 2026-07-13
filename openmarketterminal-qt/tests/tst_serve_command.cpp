#include <QtTest>
#include <QTemporaryDir>
#include <QCoreApplication>
#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"

using namespace openmarketterminal::cli;

// Exercises the serve_status/serve_stop decision logic against a synthetic
// bridge.json. HOME is redirected to a QTemporaryDir so profile_root_for(...)
// resolves under the temp tree (never touching the real profile dir). We never
// SIGTERM self: only the refuse-gui and not-running branches plus the
// live-daemon "running" status are asserted; the actual kill is covered e2e.
class TstServeCommand : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        // Prove the redirect took effect BEFORE any write/remove: if HOME did not
        // apply, profile_root_for("default") would resolve to the REAL profile
        // root and the writes below would clobber a live instance's bridge.json.
        QVERIFY(profile_root_for("default").startsWith(home_.path()));
    }

    void stop_refuses_gui_owner() {
        const QString root = profile_root_for("default");
        BridgeInfo in{"http://127.0.0.1:1", "tok", QCoreApplication::applicationPid(),
                      "2026-06-14T00:00:00Z", "gui"};      // live, but a GUI
        QVERIFY(write_bridge_file(root, in));
        QCOMPARE(serve_stop("default"), 3);                // refuse: owner is a gui
        remove_bridge_file(root);
    }

    void status_not_running_when_pid_dead() {
        const QString root = profile_root_for("default");
        BridgeInfo in{"http://127.0.0.1:1", "tok", 999999999,
                      "2026-06-14T00:00:00Z", "daemon"};   // daemon, but dead pid
        QVERIFY(write_bridge_file(root, in));
        QCOMPARE(serve_status("default", /*json=*/false), 3);
        QCOMPARE(serve_status("default", /*json=*/true), 3);
        remove_bridge_file(root);
    }

    void status_running_when_live_daemon() {
        const QString root = profile_root_for("default");
        BridgeInfo in{"http://127.0.0.1:1", "tok", QCoreApplication::applicationPid(),
                      "2026-06-14T00:00:00Z", "daemon"};   // live daemon
        QVERIFY(write_bridge_file(root, in));
        QCOMPARE(serve_status("default", /*json=*/false), 0);
        remove_bridge_file(root);
    }

    void kalshi_watchdog_detects_stale_or_disconnected_stream() {
        QVERIFY(!kalshi_event_stream_needs_recovery(false, true, 20, 60000, 10000));
        QVERIFY(!kalshi_event_stream_needs_recovery(true, true, 0, 60000, 10000));
        QVERIFY(!kalshi_event_stream_needs_recovery(true, true, 20, 9999, 10000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, false, 20, 1000, 10000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, true, 20, -1, 10000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, true, 20, 10001, 10000));
    }

    void kalshi_watchdog_bounds_market_universe_requests() {
        QVERIFY(!kalshi_universe_request_timed_out(false, 60000, 15000));
        QVERIFY(!kalshi_universe_request_timed_out(true, 15000, 15000));
        QVERIFY(kalshi_universe_request_timed_out(true, 15001, 15000));
    }
};
QTEST_MAIN(TstServeCommand)
#include "tst_serve_command.moc"
