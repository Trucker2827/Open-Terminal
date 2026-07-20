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
        QVERIFY(!kalshi_event_stream_needs_recovery(false, true, 20, 60000, 45000));
        QVERIFY(!kalshi_event_stream_needs_recovery(true, true, 0, 60000, 45000));
        QVERIFY(!kalshi_event_stream_needs_recovery(true, true, 20, 30000, 45000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, false, 20, 1000, 45000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, true, 20, -1, 45000));
        QVERIFY(kalshi_event_stream_needs_recovery(true, true, 20, 45001, 45000));
    }

    void kalshi_watchdog_refreshes_live_account_state_without_account_events() {
        QVERIFY(!kalshi_account_reconciliation_due(false, false, 0, 30000, 30000));
        QVERIFY(!kalshi_account_reconciliation_due(true, true, 0, 30000, 30000));
        QVERIFY(kalshi_account_reconciliation_due(true, false, 0, 30000, 30000));
        QVERIFY(!kalshi_account_reconciliation_due(true, false, 1000, 30999, 30000));
        QVERIFY(kalshi_account_reconciliation_due(true, false, 1000, 31000, 30000));
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
        QCOMPARE(args.at(args.indexOf("--timeout-ms") + 1), QStringLiteral("9000"));
    }

    void kalshi_persists_fresh_independent_spot_without_raw_socket_flooding() {
        QVERIFY(kalshi_should_persist_independent_spot_tick("coinbase", 64000.0, 1000, 0, 1000));
        QVERIFY(kalshi_should_persist_independent_spot_tick("kraken", 64000.0, 2000, 1000, 1000));
        QVERIFY(kalshi_should_persist_independent_spot_tick("gemini", 64000.0, 2000, 0, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("coinbase", 64000.0, 1999, 1000, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("binanceperp", 64000.0, 2000, 0, 1000));
        QVERIFY(!kalshi_should_persist_independent_spot_tick("coinbase", 0.0, 2000, 0, 1000));
    }

    void kalshi_spot_microstructure_confirms_only_matching_fresh_two_source_pressure() {
        const qint64 now = 1'000'000;
        const QJsonObject snapshot{
            {"observed_at_ms", QString::number(now - 100)},
            {"spot_microstructure", QJsonObject{
                {"direction", "up"}, {"confidence", 0.62},
                {"book_pressure", 0.35}, {"tape_pressure", 0.74},
                {"aggressor_pressure", 0.55}, {"aggressor_coverage", 0.90},
                {"classified_trades", 12},
                {"cross_source_spread_bps", 1.8}, {"live_sources", 2},
                {"top_book_sources", 2}, {"freshest_age_ms", "80"}}},
            {"flow", QJsonObject{{"divergence", QJsonObject{{"short_flow_pressure", 0.10}}}}}};
        const auto yes = kalshi_spot_microstructure_confirmation(snapshot, "yes", now);
        QVERIFY(yes.eligible);
        QVERIFY(yes.blockers.isEmpty());
        QCOMPARE(yes.to_json().value("role").toString(),
                 QStringLiteral("confirmation_or_veto_only"));

        const auto no = kalshi_spot_microstructure_confirmation(snapshot, "no", now);
        QVERIFY(!no.eligible);
        QVERIFY(no.blockers.join(' ').contains("does not confirm"));
    }

    void kalshi_spot_microstructure_fails_closed_on_stale_disagreement_or_opposing_flow() {
        const qint64 now = 1'000'000;
        const QJsonObject snapshot{
            {"observed_at_ms", QString::number(now - 6000)},
            {"spot_microstructure", QJsonObject{
                {"direction", "up"}, {"confidence", 0.60},
                {"book_pressure", 0.30}, {"tape_pressure", 0.60},
                {"aggressor_pressure", -0.60}, {"aggressor_coverage", 0.90},
                {"classified_trades", 12},
                {"cross_source_spread_bps", 15.0}, {"live_sources", 1},
                {"top_book_sources", 1}, {"freshest_age_ms", "4000"}}},
            {"flow", QJsonObject{{"divergence", QJsonObject{{"short_flow_pressure", -0.40}}}}}};
        const auto result = kalshi_spot_microstructure_confirmation(snapshot, "yes", now);
        QVERIFY(!result.eligible);
        const QString blockers = result.blockers.join(' ');
        QVERIFY(blockers.contains("stale"));
        QVERIFY(blockers.contains("fewer than two"));
        QVERIFY(blockers.contains("12 bps"));
        QVERIFY(blockers.contains("aggressor flow"));
        QVERIFY(blockers.contains("Kalshi contract flow"));
    }

    void kalshi_flow_meter_reports_confirmed_yes_pressure_from_live_contract_flow() {
        const qint64 now = 1'000'000;
        const auto metrics = kalshi_flow_metrics(
            {{0.60, 20.0}, {0.59, 15.0}}, {{0.40, 5.0}, {0.39, 5.0}},
            {{now - 1'000, "yes", 12.0}, {now - 2'000, "yes", 8.0},
             {now - 3'000, "yes", 6.0}},
            {{now - 1'500, "yes", 10.0}, {now - 2'500, "yes", 5.0},
             {now - 3'500, "no", -4.0}, {now - 4'000, "yes", 2.0},
             {now - 4'500, "yes", 2.0}, {now - 5'000, "yes", 2.0}}, now);
        QCOMPARE(metrics.signal, QStringLiteral("YES PRESSURE"));
        QVERIFY(metrics.combined_pressure >= 0.25);
        QCOMPARE(metrics.recent_trade_count, 3);
        QCOMPARE(kalshi_flow_to_json(metrics).value("advisory_only").toBool(), true);
    }

    void kalshi_flow_meter_excludes_stale_or_one_sided_activity() {
        const qint64 now = 1'000'000;
        const auto metrics = kalshi_flow_metrics(
            {{0.60, 10.0}}, {{0.40, 10.0}},
            {{now - 31'000, "yes", 999.0}},
            {{now - 31'000, "yes", 999.0}}, now);
        QCOMPARE(metrics.signal, QStringLiteral("MIXED"));
        QCOMPARE(metrics.recent_trade_count, 0);
        QCOMPARE(metrics.recent_delta_count, 0);
        QCOMPARE(metrics.confidence, QStringLiteral("LOW"));
    }

    void kalshi_flow_meter_provides_consistent_multi_window_context() {
        const qint64 now = 1'000'000;
        const auto windows = kalshi_flow_windows_to_json(
            {{0.60, 20.0}}, {{0.40, 10.0}},
            {{now - 1'000, "yes", 4.0}, {now - 45'000, "no", 90.0}},
            {{now - 1'000, "yes", 2.0}, {now - 45'000, "no", 30.0}}, now);
        QVERIFY(windows.contains(QStringLiteral("2s")));
        QVERIFY(windows.contains(QStringLiteral("30s")));
        QVERIFY(windows.contains(QStringLiteral("5m")));
        QCOMPARE(windows.value(QStringLiteral("2s")).toObject().value("trade_count").toInt(), 1);
        QCOMPARE(windows.value(QStringLiteral("30s")).toObject().value("trade_count").toInt(), 1);
        QCOMPARE(windows.value(QStringLiteral("5m")).toObject().value("trade_count").toInt(), 2);
        QCOMPARE(windows.value(QStringLiteral("5m")).toObject().value("advisory_only").toBool(), true);
    }

    void kalshi_flow_meter_marks_flow_price_divergence_as_advisory() {
        KalshiFlowMetrics pressure;
        pressure.combined_pressure = -0.4;
        const auto row = kalshi_flow_divergence_to_json(12.0, -1.2, pressure);
        QCOMPARE(row.value("label").toString(), QStringLiteral("DIVERGENCE"));
        QVERIFY(row.value("price_diverges").toBool());
        QVERIFY(row.value("flow_diverges").toBool());
        QCOMPARE(row.value("advisory_only").toBool(), true);
    }

    void kalshi_flow_meter_shows_real_immediate_taker_cost() {
        const auto economics = kalshi_flow_execution_to_json(
            {0.55, 0.57, 12.0, 8.0}, {0.43, 0.45, 10.0, 9.0}, 0.01, 0.01);
        const auto yes = economics.value("yes").toObject();
        const auto no = economics.value("no").toObject();
        QCOMPARE(yes.value("entry_cash_cost").toDouble(), 0.58);
        QCOMPARE(yes.value("immediate_exit_proceeds").toDouble(), 0.54);
        QCOMPARE(yes.value("immediate_round_trip_loss").toDouble(), 0.04);
        QCOMPARE(no.value("immediate_round_trip_loss").toDouble(), 0.04);
        QCOMPARE(economics.value("advisory_only").toBool(), true);
    }

    void kalshi_signal_transition_requires_persistent_confirmation() {
        auto first = kalshi_signal_transition("WARMING", {}, 0, "CONFIRMED_DOWN", 3);
        QCOMPARE(first.state, QStringLiteral("WARMING"));
        QCOMPARE(first.consecutive, 1);
        auto second = kalshi_signal_transition(first.state, first.pending_state,
                                               first.consecutive, "CONFIRMED_DOWN", 3);
        QCOMPARE(second.state, QStringLiteral("WARMING"));
        auto third = kalshi_signal_transition(second.state, second.pending_state,
                                              second.consecutive, "CONFIRMED_DOWN", 3);
        QCOMPARE(third.state, QStringLiteral("CONFIRMED_DOWN"));
        QVERIFY(third.changed);
        auto noise = kalshi_signal_transition(third.state, {}, 0, "CONFIRMED_UP", 3);
        QCOMPARE(noise.state, QStringLiteral("CONFIRMED_DOWN"));
        QVERIFY(!noise.changed);
    }

    void kalshi_contract_horizon_exposes_strike_time_and_required_move() {
        const auto horizon = kalshi_contract_horizon(65'000.0, 65'500.0, 0.0,
                                                     45, -12.0, "CF Benchmarks BRTI");
        QCOMPARE(horizon.value("settlement_band").toString(), QStringLiteral("final_60s"));
        QCOMPARE(horizon.value("distance_from_strike").toDouble(), -500.0);
        QVERIFY(horizon.value("required_move_bps").toDouble() > 76.0);
        QVERIFY(horizon.value("eligible_context").toBool());
        QCOMPARE(horizon.value("settlement_source").toString(),
                 QStringLiteral("CF Benchmarks BRTI"));
    }
};
QTEST_MAIN(TstServeCommand)
#include "tst_serve_command.moc"
