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

    void kalshi_watchdog_bounds_planner_processes() {
        QVERIFY(!kalshi_planner_process_timed_out(false, 60000, 25000));
        QVERIFY(!kalshi_planner_process_timed_out(true, -1, 25000));
        QVERIFY(!kalshi_planner_process_timed_out(true, 25000, 25000));
        QVERIFY(kalshi_planner_process_timed_out(true, 25001, 25000));
    }

    void kalshi_watchdog_bounds_non_execution_processes() {
        QVERIFY(!kalshi_non_execution_process_timed_out(false, 60000, 25000));
        QVERIFY(!kalshi_non_execution_process_timed_out(true, -1, 25000));
        QVERIFY(!kalshi_non_execution_process_timed_out(true, 25000, 25000));
        QVERIFY(kalshi_non_execution_process_timed_out(true, 25001, 25000));
    }

    void kalshi_event_cycle_paces_paper_but_not_armed_live() {
        QCOMPARE(kalshi_event_cycle_delay_ms(true, true, 0), 3000LL);
        QCOMPARE(kalshi_event_cycle_delay_ms(true, true, 2999), 1LL);
        QCOMPARE(kalshi_event_cycle_delay_ms(true, true, 3000), 0LL);
        QCOMPARE(kalshi_event_cycle_delay_ms(false, true, 0), 15000LL);
        QCOMPARE(kalshi_event_cycle_delay_ms(false, true, 14999), 1LL);
        QCOMPARE(kalshi_event_cycle_delay_ms(false, true, 15000), 0LL);
    }

    void kalshi_event_planner_is_bounded_to_executable_cohorts() {
        const QStringList args = kalshi_event_planner_args();
        QCOMPARE(args.mid(0, 3), QStringList({"kalshi", "auto", "run"}));
        QCOMPARE(args.at(args.indexOf("--category") + 1), QStringLiteral("Crypto#BTC@live"));
        QCOMPARE(args.at(args.indexOf("--limit") + 1), QStringLiteral("12"));
        QCOMPARE(args.at(args.indexOf("--timeout-ms") + 1), QStringLiteral("12000"));
    }

    void kalshi_persists_fresh_independent_spot_without_raw_socket_flooding() {
        QVERIFY(kalshi_should_persist_independent_spot_tick("coinbase", 64000.0, 1000, 0, 1000));
        QVERIFY(kalshi_should_persist_independent_spot_tick("kraken", 64000.0, 2000, 1000, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("coinbase", 64000.0, 1999, 1000, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("binanceperp", 64000.0, 2000, 0, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("coinbase", 0.0, 2000, 0, 1000));
    }
};
QTEST_MAIN(TstServeCommand)
#include "tst_serve_command.moc"
