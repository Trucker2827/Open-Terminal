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

    void kalshi_watchdog_distinguishes_liveness_from_market_quiet() {
        constexpr qint64 dead_after_ms = 65000;
        QVERIFY(!kalshi_event_stream_needs_recovery(
            false, true, 20, 120000, 120000, dead_after_ms));
        QVERIFY(!kalshi_event_stream_needs_recovery(
            true, true, 0, 120000, 120000, dead_after_ms));

        // Regression: a recent pong keeps a connected, quiet market healthy.
        // Quote freshness remains an independent downstream execution gate.
        QVERIFY(!kalshi_event_stream_needs_recovery(
            true, true, 20, 10000, 120000, dead_after_ms));
        QVERIFY(!kalshi_event_stream_needs_recovery(
            true, true, 20, 10000, -1, dead_after_ms));

        QVERIFY(kalshi_event_stream_needs_recovery(
            true, false, 20, 1000, 1000, dead_after_ms));
        QVERIFY(kalshi_event_stream_needs_recovery(
            true, true, 20, -1, 1000, dead_after_ms));
        QVERIFY(kalshi_event_stream_needs_recovery(
            true, true, 20, dead_after_ms + 1, 1000, dead_after_ms));
    }

    void daemon_tick_sampling_preserves_event_loop_fairness() {
        QVERIFY(daemon_tick_sample_due(1'000, 0, 50));
        QVERIFY(!daemon_tick_sample_due(1'049, 1'000, 50));
        QVERIFY(daemon_tick_sample_due(1'050, 1'000, 50));
        QVERIFY(daemon_tick_sample_due(900, 1'000, 50)); // clock/source reset
        QVERIFY(daemon_tick_sample_due(1'001, 1'000, 0));
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

    // GUI daemon indicator: the badge derives from this pure classifier so
    // indicator truth never depends on the CLI binary being present.
    void daemon_indicator_classifies_evidence_freshness() {
        const auto boundary = kalshi_daemon_evidence_freshness(70'000, 100'000);
        QCOMPARE(boundary.state, QStringLiteral("fresh"));   // 30s default, inclusive
        QCOMPARE(boundary.age_ms, 30'000LL);
        QCOMPARE(kalshi_daemon_evidence_freshness(100'000, 100'000).state,
                 QStringLiteral("fresh"));                   // heartbeat this instant
        const auto stale = kalshi_daemon_evidence_freshness(69'999, 100'000);
        QCOMPARE(stale.state, QStringLiteral("stale"));      // one ms past the window
        QCOMPARE(stale.age_ms, 30'001LL);
        QCOMPARE(kalshi_daemon_evidence_freshness(95'000, 100'000, 1'000).state,
                 QStringLiteral("stale"));                   // window is a parameter
        // No heartbeat reads as none, never fabricated: absent, negative, and
        // future timestamps are all mistrusted.
        const auto missing = kalshi_daemon_evidence_freshness(0, 100'000);
        QCOMPARE(missing.state, QStringLiteral("none"));
        QCOMPARE(missing.age_ms, -1LL);
        QCOMPARE(kalshi_daemon_evidence_freshness(-5, 100'000).state, QStringLiteral("none"));
        QCOMPARE(kalshi_daemon_evidence_freshness(100'001, 100'000).state,
                 QStringLiteral("none"));
    }

    void daemon_indicator_reads_writer_heartbeat_shape() {
        // write_book_snapshot() serializes updated_at_ms as a decimal string.
        QCOMPARE(kalshi_ws_books_heartbeat_ms(
                     QJsonObject{{"updated_at_ms", QStringLiteral("1712345678901")}}),
                 1712345678901LL);
        // A numeric value survives a future schema change.
        QCOMPARE(kalshi_ws_books_heartbeat_ms(QJsonObject{{"updated_at_ms", 1234.0}}), 1234LL);
        // Absent or junk reads as no heartbeat.
        QCOMPARE(kalshi_ws_books_heartbeat_ms(QJsonObject{}), 0LL);
        QCOMPARE(kalshi_ws_books_heartbeat_ms(
                     QJsonObject{{"updated_at_ms", QStringLiteral("not-a-number")}}), 0LL);
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

    void unchanged_kalshi_book_receipt_refreshes_evidence_without_waking_planner() {
        QJsonObject snapshots;
        QHash<QString, QString> signatures;
        const auto first = kalshi_book_receipt({}, "KXBTC:YES", 0.51, 0.53,
                                               12.0, 9.0, 1'700'000'000, 10'000);
        QVERIFY(first.meaningful_change);
        QVERIFY(kalshi_accept_book_receipt(
            snapshots, signatures, "KXBTC:YES", first,
            KalshiBookReceiptAuthority::PlannerTrigger));
        QCOMPARE(snapshots.value("KXBTC:YES").toObject()
                     .value("observed_at_ms").toString(), QStringLiteral("10000"));
        QCOMPARE(snapshots.value("KXBTC:YES").toObject()
                     .value("exchange_observed_at_ms").toString(),
                 QStringLiteral("1700000000000"));
        QCOMPARE(signatures.value("KXBTC:YES"), first.signature);

        // Exact same executable signature received later: evidence freshness
        // advances, but the deterministic planner is not woken again.
        const auto repeated = kalshi_book_receipt(first.signature, "KXBTC:YES",
                                                  0.51, 0.53, 12.0, 9.0,
                                                  1'700'000'000, 25'000);
        QVERIFY(!kalshi_accept_book_receipt(
            snapshots, signatures, "KXBTC:YES", repeated,
            KalshiBookReceiptAuthority::PlannerTrigger));
        QCOMPARE(snapshots.value("KXBTC:YES").toObject()
                     .value("observed_at_ms").toString(),
                 QStringLiteral("25000"));
        QCOMPARE(snapshots.value("KXBTC:YES").toObject().value("bid").toDouble(), 0.51);

        // A conflicting REST fallback may refresh evidence, but it cannot
        // mutate WebSocket trigger state or wake the planner.
        const auto rest = kalshi_book_receipt(first.signature, "KXBTC:YES", 0.50, 0.54,
                                              0.5, 0.5, 1'700'000'001, 30'000,
                                              "kalshi_rest");
        QVERIFY(rest.meaningful_change);
        QVERIFY(!kalshi_accept_book_receipt(
            snapshots, signatures, "KXBTC:YES", rest,
            KalshiBookReceiptAuthority::FreshnessOnly));
        QCOMPARE(signatures.value("KXBTC:YES"), first.signature);
        const QJsonObject refreshed = snapshots.value("KXBTC:YES").toObject();
        QCOMPARE(refreshed.value("observed_at_ms").toString(), QStringLiteral("30000"));
        QCOMPARE(refreshed.value("source").toString(), QStringLiteral("kalshi_rest"));
    }
    void scalp_style_normalize_and_defaults() {
        using namespace openmarketterminal::cli;
        // Empty -> scalp (the engine's historical behavior); aliases -> spot.
        QCOMPARE(scalp_style_normalize(""), QStringLiteral("scalp"));
        QCOMPARE(scalp_style_normalize("  SCALP "), QStringLiteral("scalp"));
        QCOMPARE(scalp_style_normalize("spot"), QStringLiteral("spot"));
        QCOMPARE(scalp_style_normalize("spot-swing"), QStringLiteral("spot"));
        QCOMPARE(scalp_style_normalize("swing"), QStringLiteral("spot"));
        QCOMPARE(scalp_style_normalize("intrahour"), QStringLiteral("spot"));
        QCOMPARE(scalp_style_normalize("buy-sell"), QStringLiteral("spot"));
        // Unknown -> empty so callers fail loudly instead of silently scalping.
        QVERIFY(scalp_style_normalize("yolo").isEmpty());
        // Style-dependent defaults (spot waits for bigger moves).
        QCOMPARE(scalp_style_default_min_profit_bps("scalp"), 10.0);
        QCOMPARE(scalp_style_default_min_profit_bps("spot"), 25.0);
        QCOMPARE(scalp_style_default_capture_ratio("scalp"), 0.35);
        QCOMPARE(scalp_style_default_capture_ratio("spot"), 0.55);
    }

};
QTEST_MAIN(TstServeCommand)
#include "tst_serve_command.moc"
