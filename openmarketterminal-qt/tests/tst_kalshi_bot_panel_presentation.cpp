#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimeZone>

#include "screens/kalshi/KalshiBotPanelPresentation.h"

using namespace openmarketterminal::screens::kalshi;

namespace {

QJsonObject decision_row(qint64 ts_ms, const QString& ticker, const QString& reason,
                         bool trusted = true) {
    return QJsonObject{{"event", "kalshi_bot_decision"},
                       {"ts_ms", double(ts_ms)},
                       {"mode", "paper"},
                       {"live_eligible", false},
                       {"ticker", ticker},
                       {"action", "pass"},
                       {"reason_code", reason},
                       {"signal_trusted", trusted}};
}

QJsonObject bid_row(qint64 ts_ms, const QString& ticker) {
    QJsonObject row = decision_row(ts_ms, ticker, "EDGE_CLEARS_THRESHOLD");
    row.insert("action", "bid");
    row.insert("side", "YES");
    row.insert("price", 0.42);
    row.insert("contracts", 4);
    row.insert("stake_usd", 1.68);
    row.insert("fee_usd", 0.12);
    row.insert("all_in_usd", 1.80);
    row.insert("edge", 0.134);
    row.insert("calibrated_p", 0.554);
    row.insert("market_mid", 0.42);
    row.insert("position_id", ticker + "@" + QString::number(ts_ms));
    return row;
}

QJsonObject criterion(const QString& id, double observed, double required, const QString& comparison,
                      bool met) {
    return QJsonObject{{"id", id},   {"met", met},           {"observed", observed},
                       {"required", required}, {"comparison", comparison}};
}

/// A verdict shaped exactly like KalshiBotGate::evaluate() writes it.
QJsonObject evaluated_gate(const QString& verdict, int settled, double net_pnl, bool brier_met) {
    QJsonObject brier{{"id", "brier_beats_market"},
                      {"met", brier_met},
                      {"brier_available", true},
                      {"scored_contracts", settled},
                      {"observed", 0.2101},
                      {"brier_bot", 0.2101},
                      {"brier_market_baseline", 0.2400},
                      {"brier_margin", 0.0},
                      {"required", 0.2400},
                      {"comparison", "brier_bot < brier_market_baseline - brier_margin"}};
    return QJsonObject{
        {"schema", 1},
        {"event", "kalshi_bot_gate"},
        {"verdict", verdict},
        {"evaluated", true},
        {"criteria",
         QJsonArray{criterion("min_settled_bids", settled, 300, "observed >= required",
                              settled >= 300),
                    criterion("net_pnl_usd", net_pnl, 0.0, "observed > required", net_pnl > 0.0),
                    brier,
                    criterion("max_drawdown_usd", 0.5, 5.0, "observed <= required", true)}},
        {"ledger", QJsonObject{{"settled_bids", settled},
                               {"wins", 7},
                               {"losses", 5},
                               {"net_pnl_usd", net_pnl},
                               {"fees_usd", 0.36},
                               {"max_drawdown_usd", 0.5},
                               {"scored_contracts", settled},
                               {"unscored_contracts", 0}}}};
}

QJsonObject armed_session() {
    return QJsonObject{{"session_active", true},
                       {"kill_switch", false},
                       {"max_orders_per_hour", 10},
                       {"per_bet_contract_stake_cap", 2.0},
                       {"per_bet_all_in_tolerance", 3.0},
                       {"worst_case_exposure_used", 4.5},
                       {"experiment_cap", 120.0}};
}

} // namespace

class KalshiBotPanelPresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    // --- status freshness ---------------------------------------------------
    void empty_ledger_reads_off_grey() {
        const auto view = present_kalshi_bot_panel({}, {}, {}, 1'000'000);
        QCOMPARE(view.state, QStringLiteral("off"));
        QCOMPARE(kalshi_bot_state_color_role(view.state), QStringLiteral("grey"));
        QVERIFY(view.status.contains("BOT OFF"));
        QVERIFY(view.status.contains("kalshi-bot-decisions.jsonl"));
        QVERIFY(view.decisions.isEmpty());
    }

    void fresh_ledger_reads_running_green() {
        const qint64 now = 2'000'000'000;
        const QJsonArray rows{decision_row(now - 30'000, "KXBTCD-A", "EDGE_BELOW_THRESHOLD")};
        const auto view = present_kalshi_bot_panel(rows, {}, {}, now);
        QCOMPARE(view.state, QStringLiteral("running"));
        QCOMPARE(kalshi_bot_state_color_role(view.state), QStringLiteral("green"));
        QVERIFY(view.status.contains("BOT RUNNING"));
        QVERIFY(view.status.contains("30s ago"));
    }

    void stale_ledger_reads_stale_amber() {
        const qint64 now = 3'000'000'000;
        const QJsonArray rows{
            decision_row(now - kKalshiBotStaleMs - 1, "KXBTCD-A", "EDGE_BELOW_THRESHOLD")};
        const auto view = present_kalshi_bot_panel(rows, {}, {}, now);
        QCOMPARE(view.state, QStringLiteral("stale"));
        QCOMPARE(kalshi_bot_state_color_role(view.state), QStringLiteral("amber"));
        QVERIFY(view.status.contains("BOT STALE"));
        QVERIFY(!view.status.contains("BOT RUNNING"));
    }

    void future_timestamp_is_mistrusted_not_read_as_fresh() {
        const qint64 now = 3'500'000'000;
        const QJsonArray rows{decision_row(now + 60'000, "KXBTCD-A", "EDGE_BELOW_THRESHOLD")};
        const auto view = present_kalshi_bot_panel(rows, {}, {}, now);
        QCOMPARE(view.state, QStringLiteral("stale"));
        QVERIFY(view.status.contains("future"));
    }

    // --- the kill switch (issue #129) ---------------------------------------

    /// The chip must not paint green on a bot that has been killed. The last
    /// row a stopped loop writes is its own BOT_STOPPED refusal, which is as
    /// fresh as any other tick.
    void an_engaged_kill_switch_beats_a_fresh_ledger() {
        const qint64 now = 3'600'000'000;
        const QJsonArray rows{decision_row(now - 1'000, "", "BOT_STOPPED")};
        KalshiBotStopFile stop;
        stop.engaged = true;
        stop.ts_ms = now - 2'000;
        stop.source = QStringLiteral("gui");
        const auto view = present_kalshi_bot_panel(rows, {}, {}, now, 8, stop);
        QCOMPARE(view.state, QStringLiteral("stopped"));
        QVERIFY(view.stopped);
        QCOMPARE(kalshi_bot_state_color_role(view.state), QStringLiteral("red"));
        QVERIFY(view.status.contains("BOT STOPPED"));
        QVERIFY(!view.status.contains("BOT RUNNING"));
        QVERIFY(view.status.contains("by gui"));
        // Same ledger, switch cleared: it reads running again, so the stopped
        // verdict came from the switch and not from the rows.
        const auto cleared = present_kalshi_bot_panel(rows, {}, {}, now);
        QCOMPARE(cleared.state, QStringLiteral("running"));
        QVERIFY(!cleared.stopped);
    }

    /// A stopped tick never opened the calibrator report, so it has no opinion
    /// on the signal — and must not be described as having refused one.
    void a_stopped_tick_says_the_signal_was_not_read() {
        const qint64 now = 3'700'000'000;
        KalshiBotStopFile stop;
        stop.engaged = true;
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 1'000, "", "BOT_STOPPED")}, {}, {}, now, 8, stop);
        QVERIFY(view.signal.contains("SIGNAL NOT READ"));
        QVERIFY(!view.signal.contains("refused its report"));
    }

    /// The panel and the CLI resolve the stop file through the same path
    /// module, so the button cannot write a switch the loop never reads.
    void the_stop_file_resolves_through_the_shared_evidence_path() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR", dir.path().toUtf8());
        QCOMPARE(kalshi_bot_stop_path(),
                 QDir(dir.path()).filePath(QStringLiteral("kalshi-bot-stop.json")));
        QVERIFY(!read_kalshi_bot_stop_file().engaged);
        qunsetenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR");
    }

    // --- decision rows ------------------------------------------------------
    void bid_row_shows_side_price_size_edge_and_reason() {
        const qint64 now = 4'000'000'000;
        const auto view = present_kalshi_bot_panel(QJsonArray{bid_row(now - 5'000, "KXBTCD-B118")},
                                                   {}, {}, now);
        QCOMPARE(view.decisions.size(), 1);
        const QString line = view.decisions.first();
        QVERIFY(line.contains("BID"));
        QVERIFY(line.contains("KXBTCD-B118"));
        QVERIFY(line.contains("YES $0.42 x4"));
        QVERIFY(line.contains("all-in $1.80"));
        QVERIFY(line.contains("edge +0.134"));
        QVERIFY(line.contains("EDGE_CLEARS_THRESHOLD"));
    }

    // Ladder rung 6 put the order lifecycle in the same ledger. A canceled
    // quote rendered as a "PASS" would be a lie on the screen, and a bid that
    // is still only a resting order must not read like a held position.
    void a_resting_bid_says_it_is_resting() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = bid_row(now - 5'000, "KXBTCD-B118");
        row.insert("order_state", "resting");
        const auto view = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now);
        QVERIFY(view.decisions.first().contains("BID"));
        QVERIFY(view.decisions.first().contains("resting"));
    }

    /// Issue #158: two bids of the same size at the same contract are
    /// different acts depending on whether they PAID the spread. The operator
    /// watching the fill rate move has to be able to tell them apart, and a
    /// row from a build that quoted only one way must not claim either.
    void a_bid_row_says_which_tier_quoted_it() {
        const qint64 now = 4'000'000'000;
        QJsonObject crossed = bid_row(now - 5'000, "KXBTCD-B118");
        crossed.insert("order_state", "resting");
        crossed.insert("quote_style", "cross");
        QVERIFY(present_kalshi_bot_panel(QJsonArray{crossed}, {}, {}, now)
                    .decisions.first()
                    .contains("cross"));

        QJsonObject rested = bid_row(now - 5'000, "KXBTCD-B118");
        rested.insert("quote_style", "rest");
        QVERIFY(present_kalshi_bot_panel(QJsonArray{rested}, {}, {}, now)
                    .decisions.first()
                    .contains("rest"));

        // A row written before this rung says nothing about a tier, so the
        // line says nothing either — no default is invented for it.
        const QString silent = present_kalshi_bot_panel(
            QJsonArray{bid_row(now - 5'000, "KXBTCD-B118")}, {}, {}, now).decisions.first();
        QVERIFY(!silent.contains("cross"));
        QVERIFY(!silent.contains("rest"));
    }

    void a_cancel_is_shown_as_a_cancel_and_never_as_a_pass() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = decision_row(now - 5'000, "KXBTCD-B118", "CANCELED_TTL");
        row.insert("action", "cancel");
        row.insert("side", "YES");
        row.insert("limit_price", 0.42);
        row.insert("contracts", 4);
        const auto view = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now);
        const QString line = view.decisions.first();
        QVERIFY(line.contains("CANCEL"));
        QVERIFY(!line.contains("PASS"));
        QVERIFY(line.contains("YES $0.42 x4"));
        QVERIFY(line.contains("CANCELED_TTL"));
    }

    void an_unconfirmed_cancel_says_the_money_is_still_at_risk() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = decision_row(now - 5'000, "KXBTCD-B118", "UNCONFIRMED_CANCEL");
        row.insert("action", "cancel");
        row.insert("contracts", 4);
        row.insert("limit_price", 0.42);
        row.insert("still_at_risk_usd", 1.68);
        const auto view = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now);
        QVERIFY(view.decisions.first().contains("STILL AT RISK $1.68"));
    }

    void a_fill_is_shown_as_a_fill() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = decision_row(now - 5'000, "KXBTCD-B118", "FILLED_AT_LIMIT");
        row.insert("action", "fill");
        row.insert("side", "YES");
        row.insert("price", 0.42);
        row.insert("contracts", 4);
        const auto view = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now);
        QVERIFY(view.decisions.first().contains("FILL"));
        QVERIFY(!view.decisions.first().contains("PASS"));
        QVERIFY(view.decisions.first().contains("YES $0.42 x4"));
    }

    void passes_are_shown_and_carry_no_invented_numbers() {
        const qint64 now = 4'500'000'000;
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 1'000, "KXBTCD-C", "NO_RUNWAY")}, {}, {}, now);
        QCOMPARE(view.decisions.size(), 1);
        const QString line = view.decisions.first();
        QVERIFY(line.contains("PASS"));
        QVERIFY(line.contains("NO_RUNWAY"));
        QVERIFY(!line.contains("$"));      // no price, size, or stake was decided
        QVERIFY(!line.contains("edge"));   // no edge was measured
    }

    void refusal_row_without_a_contract_says_so() {
        const qint64 now = 4'600'000'000;
        QJsonObject refusal = decision_row(now - 1'000, QString(), "REPORT_MISSING");
        refusal.remove("signal_trusted");
        const auto view = present_kalshi_bot_panel(QJsonArray{refusal}, {}, {}, now);
        QVERIFY(view.decisions.first().contains("(no contract)"));
        QVERIFY(view.decisions.first().contains("REPORT_MISSING"));
        QVERIFY(view.signal.contains("SIGNAL UNKNOWN"));
        QVERIFY(view.signal.contains("REPORT_MISSING"));
    }

    void newest_decision_is_first_and_the_list_is_bounded() {
        const qint64 now = 5'000'000'000;
        QJsonArray rows;
        for (int i = 0; i < 20; ++i)
            rows.append(decision_row(now - (20 - i) * 1'000,
                                     QStringLiteral("KX-%1").arg(i), "EDGE_BELOW_THRESHOLD"));
        const auto view = present_kalshi_bot_panel(rows, {}, {}, now, 8);
        QCOMPARE(view.decisions.size(), 8);
        QVERIFY(view.decisions.first().contains("KX-19"));
        QVERIFY(view.decisions.last().contains("KX-12"));
    }

    // --- signal trust -------------------------------------------------------
    void untrusted_signal_is_stated_not_hidden() {
        const qint64 now = 5'500'000'000;
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 1'000, "KXBTCD-D", "SIGNAL_UNTRUSTED", false)}, {}, {},
            now);
        QVERIFY(view.signal.contains("SIGNAL UNTRUSTED"));
    }

    void trusted_signal_reads_trusted() {
        const qint64 now = 5'600'000'000;
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 1'000, "KXBTCD-D", "EDGE_BELOW_THRESHOLD", true)}, {}, {},
            now);
        QVERIFY(view.signal.contains("SIGNAL TRUSTED"));
    }

    // --- gate + scoreboard --------------------------------------------------
    void missing_gate_claims_no_scoreboard() {
        const auto view = present_kalshi_bot_panel({}, {}, {}, 6'000'000'000);
        QVERIFY(view.gate.contains("GATE NOT EVALUATED"));
        QVERIFY(view.gate.contains("kalshi-bot-gate.json"));
        QVERIFY(view.scoreboard.contains("UNAVAILABLE"));
        QVERIFY(!view.scoreboard.contains("$0.00"));  // absent, never zero
        QVERIFY(!view.gate_pass);
    }

    void gate_fail_renders_the_verdict_with_its_numbers() {
        const auto view = present_kalshi_bot_panel({}, evaluated_gate("FAIL", 12, -0.34, true), {},
                                                   6'100'000'000);
        QVERIFY(!view.gate_pass);
        QVERIFY(view.gate.startsWith("GATE FAIL"));
        QVERIFY(view.gate.contains("min_settled_bids 12 >= 300 NOT MET"));
        QVERIFY(view.gate.contains("net_pnl_usd -0.34 > 0.00 NOT MET"));
        QVERIFY(view.gate.contains("brier_beats_market 0.2101 < 0.2400 MET"));
        QVERIFY(view.gate.contains("max_drawdown_usd 0.50 <= 5.00 MET"));
        QVERIFY(view.scoreboard.contains("12 settled"));
        QVERIFY(view.scoreboard.contains("net $-0.34"));
        QVERIFY(view.scoreboard.contains("$0.36 fees"));
        QVERIFY(view.scoreboard.contains("Brier 0.2101 vs market 0.2400"));
    }

    void gate_pass_reads_pass() {
        const auto view = present_kalshi_bot_panel({}, evaluated_gate("PASS", 340, 2.15, true), {},
                                                   6'200'000'000);
        QVERIFY(view.gate_pass);
        QVERIFY(view.gate.startsWith("GATE PASS"));
        QVERIFY(view.gate.contains("min_settled_bids 340 >= 300 MET"));
    }

    void refused_gate_publishes_no_numbers() {
        const QJsonObject refusal{{"verdict", "TAMPERED"},
                                  {"evaluated", false},
                                  {"reason", "the gate params failed their seal check"}};
        const auto view = present_kalshi_bot_panel({}, refusal, {}, 6'300'000'000);
        QVERIFY(!view.gate_pass);
        QVERIFY(view.gate.contains("GATE TAMPERED"));
        QVERIFY(view.gate.contains("seal check"));
        QVERIFY(view.scoreboard.contains("UNAVAILABLE"));
        QVERIFY(!view.scoreboard.contains("settled ·"));
    }

    void unscoreable_brier_is_absent_not_zero() {
        QJsonObject gate = evaluated_gate("FAIL", 3, 0.10, false);
        QJsonArray criteria = gate.value("criteria").toArray();
        criteria.replace(2, QJsonObject{{"id", "brier_beats_market"},
                                        {"met", false},
                                        {"brier_available", false},
                                        {"scored_contracts", 0},
                                        {"unscored_contracts", 3},
                                        {"note", "no settled bid carried both a calibrated "
                                                 "probability and the market mid"}});
        gate.insert("criteria", criteria);
        const auto view = present_kalshi_bot_panel({}, gate, {}, 6'400'000'000);
        QVERIFY(view.scoreboard.contains("Brier NOT SCORED"));
        QVERIFY(!view.scoreboard.contains("Brier 0.0000"));
        QVERIFY(view.gate.contains("brier_beats_market NOT SCORED"));
    }

    // --- armed state and caps ----------------------------------------------
    void no_session_object_fails_closed_rather_than_claiming_disarmed() {
        const auto view = present_kalshi_bot_panel({}, {}, {}, 7'000'000'000);
        QVERIFY(!view.armed_live);
        QVERIFY(view.armed.contains("SESSION UNKNOWN"));
        QVERIFY(view.armed.contains("FAIL CLOSED"));
    }

    void inactive_session_reads_disarmed() {
        const QJsonObject status{{"session_active", false}, {"kill_switch", false}};
        const auto view = present_kalshi_bot_panel({}, {}, status, 7'100'000'000);
        QVERIFY(!view.armed_live);
        QVERIFY(view.armed.startsWith("DISARMED"));
        QVERIFY(view.armed.contains("mode=paper"));
    }

    void armed_session_shows_the_caps_in_force() {
        const auto view = present_kalshi_bot_panel({}, {}, armed_session(), 7'200'000'000);
        QVERIFY(view.armed_live);
        QVERIFY(view.armed.startsWith("ARMED"));
        // The arm on display is the shared live session's, not the bot's own —
        // the line says so, so an armed session cannot read as an armed bot.
        QVERIFY(view.armed.contains("shared Kalshi live session"));
        QVERIFY(view.armed.contains("the bot has no arm of its own"));
        QVERIFY(view.armed.contains("stake <= $2.00"));
        QVERIFY(view.armed.contains("all-in <= $3.00"));
        QVERIFY(view.armed.contains("<= 10 orders/hour"));
        QVERIFY(view.armed.contains("exposure $4.50 of $120.00"));
        QVERIFY(!view.armed.contains("KILL SWITCH"));
    }

    void a_cap_the_session_did_not_state_is_unknown_not_defaulted() {
        QJsonObject status = armed_session();
        status.remove("per_bet_contract_stake_cap");
        const auto view = present_kalshi_bot_panel({}, {}, status, 7'300'000'000);
        QVERIFY(view.armed.contains("stake <= unknown"));
        QVERIFY(!view.armed.contains("stake <= $2.00"));
    }

    // --- the LIVE badge (ladder rung 5) -------------------------------------

    void a_paper_ledger_never_shows_a_live_badge() {
        const qint64 now = 4'000'000'000;
        const auto view = present_kalshi_bot_panel(QJsonArray{bid_row(now - 5'000, "KXBTCD-B118")},
                                                   {}, {}, now);
        QVERIFY(!view.mode_live);
        QCOMPARE(view.mode, QStringLiteral("PAPER"));
        QVERIFY(view.status.startsWith("[PAPER]"));
        QVERIFY(!view.decisions.first().contains("LIVE"));
    }

    void a_live_tick_shows_the_live_badge_and_marks_its_own_rows() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = bid_row(now - 5'000, "KXBTCD-B118");
        row.insert("mode", "live");
        row.insert("live_eligible", true);
        row.insert("submit_status", "filled");
        row.insert("order_state", "filled");
        const auto view = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now);
        QVERIFY(view.mode_live);
        QCOMPARE(view.mode, QStringLiteral("LIVE"));
        QVERIFY(view.status.startsWith("[LIVE]"));
        const QString line = view.decisions.first();
        QVERIFY2(line.contains("LIVE"), qPrintable(line));
        // Whatever the submit path answered, verbatim: a refused order can
        // never read on the screen as a placed one.
        QVERIFY2(line.contains("submit filled"), qPrintable(line));
    }

    void a_live_bid_the_submit_path_refused_says_so_on_its_own_row() {
        const qint64 now = 4'000'000'000;
        QJsonObject row = bid_row(now - 5'000, "KXBTCD-B118");
        row.insert("mode", "live");
        row.insert("reason_code", "LIVE_ORDER_REJECTED_BY_SUBMIT");
        row.insert("submit_status", "rejected");
        const QString line = present_kalshi_bot_panel(QJsonArray{row}, {}, {}, now).decisions.first();
        QVERIFY2(line.contains("submit rejected"), qPrintable(line));
        QVERIFY2(line.contains("LIVE_ORDER_REJECTED_BY_SUBMIT"), qPrintable(line));
    }

    // The badge is read off the NEWEST row, exactly as the signal line is. A
    // bot that ran live an hour ago and papers now is papering, and a badge
    // that latched on any live row in the window would keep claiming otherwise.
    void the_badge_follows_the_newest_tick_not_the_whole_window() {
        const qint64 now = 4'000'000'000;
        QJsonObject old_live = bid_row(now - 600'000, "KXBTCD-OLD");
        old_live.insert("mode", "live");
        const auto view = present_kalshi_bot_panel(
            QJsonArray{old_live, bid_row(now - 5'000, "KXBTCD-NEW")}, {}, {}, now);
        QVERIFY2(!view.mode_live, "a live row an hour back does not make this tick live");
        QCOMPARE(view.mode, QStringLiteral("PAPER"));
        // The historical row still says what it was.
        QVERIFY(view.decisions.last().contains("LIVE"));
    }

    // --- schema vintage: the badge fails CLOSED (issue #145) ----------------
    //
    // The operator's screenshot showed "[LIVE] BOT STALE · last decision 15h
    // ago" over a ledger with zero live rows. A panel that reads the badge off
    // the last row it happens to understand announces a mode nothing in the
    // ledger claims, and LIVE is the worst direction to fail in.

    /// The reproduction: a live tick, then rows written by a bot binary this
    /// build does not know. The badge must not keep flying LIVE over them.
    void an_unreadable_newer_row_never_leaves_the_badge_on_live() {
        const qint64 now = 4'100'000'000;
        QJsonObject live = bid_row(now - 3'600'000, "KXBTCD-LIVE");
        live.insert("mode", "live");
        const QJsonObject unknown_vintage{{"event", "kalshi_bot_tick"},
                                          {"ts_ms", double(now - 30'000)},
                                          {"ticker", "KXBTCD-NEW"}};
        const auto view = present_kalshi_bot_panel(QJsonArray{live, unknown_vintage}, {}, {}, now);
        QVERIFY2(!view.mode_live, "an unreadable newest row is not a live tick");
        QVERIFY(view.mode_unknown);
        QCOMPARE(view.mode, QStringLiteral("UNKNOWN"));
        QVERIFY2(!view.status.contains("[LIVE]"), qPrintable(view.status));
        QVERIFY(view.status.startsWith("[UNKNOWN]"));
    }

    /// A tick row of the known event whose `mode` this build cannot read is
    /// just as unreadable: absent is not paper, and it is certainly not live.
    void a_tick_whose_mode_this_build_cannot_read_reads_unknown() {
        const qint64 now = 4'200'000'000;
        QJsonObject undeclared = decision_row(now - 10'000, "KXBTCD-NEW", "EDGE_BELOW_THRESHOLD");
        undeclared.remove("mode");
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 70'000, "KXBTCD-OLD", "EDGE_BELOW_THRESHOLD"), undeclared},
            {}, {}, now);
        QVERIFY(!view.mode_live);
        QCOMPARE(view.mode, QStringLiteral("UNKNOWN"));
        QVERIFY2(!view.mode.contains("PAPER"), "an unstated mode is unknown, not paper");
    }

    /// Freshness is not a function of schema vintage: a row this build cannot
    /// read at all still proves the loop ticked. Freezing the age at the last
    /// row that parsed is how a running bot reads as hours stale.
    void freshness_comes_from_the_newest_row_whatever_its_vintage() {
        const qint64 now = 4'300'000'000;
        // Dated the way rows have always been dated in ISO, but with no ts_ms
        // and an event this build does not know.
        const QJsonObject unknown_vintage{
            {"event", "kalshi_bot_tick"},
            {"ts", QDateTime::fromMSecsSinceEpoch(now - 30'000, QTimeZone::UTC)
                       .toString(Qt::ISODateWithMs)}};
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 3'600'000, "KXBTCD-OLD", "EDGE_BELOW_THRESHOLD"),
                       unknown_vintage},
            {}, {}, now);
        QCOMPARE(view.state, QStringLiteral("running"));
        QVERIFY2(view.status.contains("30s ago"), qPrintable(view.status));
        QVERIFY2(!view.status.contains("BOT STALE"), qPrintable(view.status));
    }

    /// The mismatch is actionable, so the panel prints the action.
    void a_vintage_mismatch_surfaces_the_kickstart_hint() {
        const qint64 now = 4'400'000'000;
        const QJsonObject unknown_vintage{{"event", "kalshi_bot_tick"},
                                          {"ts_ms", double(now - 5'000)}};
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 65'000, "KXBTCD-OLD", "EDGE_BELOW_THRESHOLD"),
                       unknown_vintage},
            {}, {}, now);
        QCOMPARE(view.vintage_hint,
                 QStringLiteral("bot binary older than GUI — launchctl kickstart -k "
                                "gui/$UID/org.openterminal.kalshi-bot"));
        QVERIFY2(view.status.contains(view.vintage_hint), qPrintable(view.status));
    }

    /// The whole point, stated as a property: no mixture of schema vintages
    /// over a ledger that never went live may produce a LIVE badge.
    void an_all_paper_ledger_never_renders_live_under_any_schema_mix() {
        const qint64 now = 4'500'000'000;
        QJsonObject no_mode = decision_row(now - 20'000, "KXBTCD-NOMODE", "EDGE_BELOW_THRESHOLD");
        no_mode.remove("mode");
        QJsonObject odd_mode = decision_row(now - 15'000, "KXBTCD-ODD", "EDGE_BELOW_THRESHOLD");
        odd_mode.insert("mode", "shadow");
        const QJsonObject unknown_event{{"event", "kalshi_bot_tick"},
                                        {"ts_ms", double(now - 10'000)}};
        const QJsonObject settlement{{"event", "kalshi_bot_paper_settlement"},
                                     {"ts_ms", double(now - 5'000)},
                                     {"mode", "paper"},
                                     {"ticker", "KXBTCD-SETTLED"}};
        const QJsonObject paper = decision_row(now - 25'000, "KXBTCD-PAPER", "EDGE_BELOW_THRESHOLD");
        const QList<QJsonArray> mixes{
            QJsonArray{paper, no_mode},
            QJsonArray{paper, odd_mode},
            QJsonArray{paper, unknown_event},
            QJsonArray{no_mode, unknown_event, settlement},
            QJsonArray{paper, no_mode, odd_mode, unknown_event, settlement},
            QJsonArray{unknown_event, paper},
        };
        for (const QJsonArray& rows : mixes) {
            const auto view = present_kalshi_bot_panel(rows, {}, {}, now);
            QVERIFY2(!view.mode_live, "a ledger with no live row cannot render a live badge");
            QVERIFY2(view.mode != QStringLiteral("LIVE"), qPrintable(view.mode));
            QVERIFY2(!view.status.contains("[LIVE]"), qPrintable(view.status));
            // And what it does render is never a mode nothing stated: the mix
            // either has a readable newest tick (PAPER) or it says UNKNOWN.
            QVERIFY2(view.mode == QStringLiteral("PAPER") || view.mode == QStringLiteral("UNKNOWN"),
                     qPrintable(view.mode));
        }
    }

    /// Failing closed must not become failing quiet in the other direction: a
    /// paper settlement written after a live tick is the same vintage and is
    /// not a tick at all, so it cannot mask real money at the exchange.
    void a_settlement_after_a_live_tick_does_not_mask_the_live_badge() {
        const qint64 now = 4'600'000'000;
        QJsonObject live = bid_row(now - 20'000, "KXBTCD-LIVE");
        live.insert("mode", "live");
        const QJsonObject settlement{{"event", "kalshi_bot_paper_settlement"},
                                     {"ts_ms", double(now - 5'000)},
                                     {"mode", "paper"},
                                     {"ticker", "KXBTCD-OLD"}};
        const auto view = present_kalshi_bot_panel(QJsonArray{live, settlement}, {}, {}, now);
        QVERIFY2(view.mode_live, "the last TICK was live; a settlement is not a tick");
        QCOMPARE(view.mode, QStringLiteral("LIVE"));
        QVERIFY(!view.mode_unknown);
    }

    /// Two appenders (the launchd loop and a hand-run `kalshi bot once`)
    /// interleave rows, so "newest" is the timestamp, not the file position.
    void the_newest_tick_is_the_newest_timestamp_not_the_last_line() {
        const qint64 now = 4'700'000'000;
        QJsonObject live_older = bid_row(now - 60'000, "KXBTCD-LIVE");
        live_older.insert("mode", "live");
        const auto view = present_kalshi_bot_panel(
            QJsonArray{decision_row(now - 10'000, "KXBTCD-NEW", "EDGE_BELOW_THRESHOLD"), live_older},
            {}, {}, now);
        QVERIFY2(!view.mode_live, "the newest tick by time papered, whatever order it was appended");
        QCOMPARE(view.mode, QStringLiteral("PAPER"));
    }

    /// Issue #155, criterion 4 — the GUI leg of CLI/GUI mode agreement.
    ///
    /// The badge is no longer the panel's own derivation: it is
    /// `kalshi_bot_mode()`, the classifier `kalshi bot status` renders too. Over
    /// ONE fixture ledger this asserts the panel's rendering IS that function's
    /// answer, in all four cases, badge and headline alike. The CLI leg — the
    /// same four ledgers through the real `openterminalcli --json kalshi bot
    /// status` — is e2e_kalshi_bot_status_mode.sh; neutering the classifier
    /// turns both red, which is the property this issue buys.
    void the_badge_is_the_shared_classifiers_answer_not_a_second_derivation() {
        using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_mode;
        const qint64 now = 4'800'000'000;

        QJsonObject live = bid_row(now - 20'000, "KXBTCD-AGREE-LIVE");
        live.insert("mode", "live");
        const QJsonObject paper = decision_row(now - 20'000, "KXBTCD-AGREE", "EDGE_BELOW_THRESHOLD");
        const QJsonObject unreadable{{"event", "kalshi_bot_tick"}, {"ts_ms", double(now - 5'000)}};
        const QJsonObject settlement{{"event", "kalshi_bot_paper_settlement"},
                                     {"ts_ms", double(now - 1'000)},
                                     {"mode", "paper"},
                                     {"position_id", "KXBTCD-AGREE@1"}};

        struct Case {
            const char* name;
            QJsonArray rows;
            QString expected;  // the word the classifier must answer, "" for none
        };
        const QList<Case> cases{
            {"live", QJsonArray{paper, live}, QStringLiteral("live")},
            {"paper", QJsonArray{live, paper}, QStringLiteral("paper")},
            {"unknown", QJsonArray{live, unreadable}, QStringLiteral("unknown")},
            // No tick at all: dated settlement rows, a running loop, and still
            // nothing that states a mode — so no badge, not a default PAPER.
            {"no tick", QJsonArray{settlement}, QString()},
        };

        for (const Case& item : cases) {
            const auto mode = kalshi_bot_mode(item.rows);
            QCOMPARE(mode.mode, item.expected);
            const auto view = present_kalshi_bot_panel(item.rows, {}, {}, now);
            // The badge IS the classifier's word, uppercased — no second rule.
            QVERIFY2(view.mode == mode.badge(),
                     qPrintable(QStringLiteral("%1: badge %2 vs classifier %3")
                                    .arg(QString::fromLatin1(item.name), view.mode, mode.badge())));
            QCOMPARE(view.mode_live, mode.live);
            QCOMPARE(view.mode_unknown, mode.unknown);
            // ...and so is the `[MODE] status` line, reason text and hint
            // included, so the window and the shell cannot word it differently.
            // The undecorated headline is the CLI's own loop classifier plus
            // the panel's decision count; everything after that is the shared
            // `kalshi_bot_mode_headline()` and nothing else.
            int decisions = 0;
            for (const auto& value : item.rows)
                if (value.toObject().value("event").toString() == "kalshi_bot_decision") ++decisions;
            const auto loop = openmarketterminal::services::prediction::kalshi_ns::
                kalshi_bot_loop_status(openmarketterminal::services::prediction::kalshi_ns::
                                           kalshi_bot_newest_ts_ms(item.rows),
                                       {}, now);
            const QString bare =
                loop.state == QStringLiteral("running")
                    ? QStringLiteral("%1 · %2 decisions in view").arg(loop.headline).arg(decisions)
                    : loop.headline;
            QCOMPARE(view.status, openmarketterminal::services::prediction::kalshi_ns::
                                      kalshi_bot_mode_headline(bare, mode));
            if (item.expected.isEmpty())
                QVERIFY2(!view.status.startsWith(QStringLiteral("[")), qPrintable(view.status));
        }
    }

    void engaged_kill_switch_is_shown() {
        QJsonObject status = armed_session();
        status.insert("kill_switch", true);
        const auto view = present_kalshi_bot_panel({}, {}, status, 7'400'000'000);
        QVERIFY(view.armed.contains("KILL SWITCH ON"));
    }

    // --- the panel reads the CLI's own files, through the one path module ---
    void panel_paths_come_from_the_unified_evidence_module() {
        QTemporaryDir evidence;
        QVERIFY(evidence.isValid());
        qputenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR", evidence.path().toUtf8());

        QCOMPARE(kalshi_bot_ledger_path(),
                 QDir(evidence.path()).filePath(QStringLiteral("kalshi-bot-decisions.jsonl")));
        QCOMPARE(kalshi_bot_gate_path(),
                 QDir(evidence.path()).filePath(QStringLiteral("kalshi-bot-gate.json")));

        const qint64 now = 8'000'000'000;
        QFile ledger(kalshi_bot_ledger_path());
        QVERIFY(ledger.open(QIODevice::WriteOnly | QIODevice::Text));
        ledger.write(QJsonDocument(decision_row(now - 120'000, "KX-OLD", "EDGE_BELOW_THRESHOLD"))
                         .toJson(QJsonDocument::Compact) + "\n");
        ledger.write(QJsonDocument(bid_row(now - 10'000, "KXBTCD-PATHS"))
                         .toJson(QJsonDocument::Compact) + "\n");
        ledger.close();

        QFile gate(kalshi_bot_gate_path());
        QVERIFY(gate.open(QIODevice::WriteOnly | QIODevice::Text));
        gate.write(QJsonDocument(evaluated_gate("FAIL", 12, -0.34, true)).toJson());
        gate.close();

        const auto view = load_kalshi_bot_panel(armed_session(), now);
        qunsetenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR");

        QCOMPARE(view.state, QStringLiteral("running"));
        QCOMPARE(view.decisions.size(), 2);
        QVERIFY(view.decisions.first().contains("KXBTCD-PATHS"));   // from the ledger file
        QVERIFY(view.gate.contains("min_settled_bids 12 >= 300"));  // from the gate file
        QVERIFY(view.scoreboard.contains("12 settled"));
        QVERIFY(view.armed_live);
    }

    void a_partial_first_line_in_the_read_window_is_dropped_not_parsed() {
        QTemporaryDir evidence;
        QVERIFY(evidence.isValid());
        qputenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR", evidence.path().toUtf8());
        const qint64 now = 8'100'000'000;
        QFile ledger(kalshi_bot_ledger_path());
        QVERIFY(ledger.open(QIODevice::WriteOnly | QIODevice::Text));
        for (int i = 0; i < 3; ++i)
            ledger.write(QJsonDocument(decision_row(now - (3 - i) * 1'000,
                                                    QStringLiteral("KX-%1").arg(i),
                                                    "EDGE_BELOW_THRESHOLD"))
                             .toJson(QJsonDocument::Compact) + "\n");
        ledger.close();
        // A window smaller than the file starts mid-row; that fragment must not
        // become a decision.
        const QJsonArray rows = read_kalshi_bot_ledger_tail(120);
        qunsetenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR");
        for (const auto& value : rows)
            QVERIFY(!value.toObject().value(QStringLiteral("ticker")).toString().isEmpty());
    }

    // --- the conversion funnel (issue #153) ---------------------------------
    // The window and `kalshi bot status` must never round differently or
    // disagree about a denominator, so the panel is a PASS-THROUGH of the one
    // formatter rather than a second derivation of the same file.

    void the_panels_funnel_lines_are_the_shared_formatters_output() {
        KalshiBotFunnelFile file;
        file.available = true;
        file.object = QJsonObject{{"ts_ms", 9'000'000'000.0},
                                  {"bids", 304},
                                  {"fills", 5},
                                  {"settlements", 4},
                                  {"resting_now", 12},
                                  {"canceled_ttl", 288},
                                  {"canceled_edge_gone", 3},
                                  {"canceled_market_settled", 1},
                                  {"unconfirmed_cancels", 0},
                                  {"rows_read", 63'305},
                                  {"span_ms", 168'480'000.0},
                                  {"fill_rate", 5.0 / 304.0},
                                  {"settled_per_day", 2.0512},
                                  {"gate_required", 300},
                                  {"settled_remaining", 296},
                                  {"days_to_gate_at_observed_rate", 144.3},
                                  {"fill_models", QJsonArray{"rung6_conditional_mid"}},
                                  // Keyed by quoting tier since #158: a mixed
                                  // record states one sentence per tier, and
                                  // the panel prints whatever the file holds.
                                  {"fill_rules",
                                   QJsonObject{{"rest",
                                                QJsonObject{{"fills", 4},
                                                            {"rule", "a stated model, not a "
                                                                     "measured fill"}}},
                                               {"cross",
                                                QJsonObject{{"fills", 1},
                                                            {"rule", "quoted AT the observed "
                                                                     "ask"}}}}}};
        const qint64 now = 9'000'060'000;
        const auto view = present_kalshi_bot_panel({}, {}, {}, now, 8, {}, file);
        // Character-for-character the same lines the CLI prints and puts in
        // `funnel_lines` — one formatter, one rounding.
        QCOMPARE(view.funnel, kalshi_bot_funnel_lines(file, now));
        QVERIFY(view.funnel.size() == 5);
        QVERIFY(view.funnel.at(0).contains(QStringLiteral("304 bids → 5 fills → 4 settled")));
        QVERIFY(view.funnel.at(1).contains(QStringLiteral("5 of 304 bids")));
        QVERIFY(view.funnel.at(3).contains(QStringLiteral("296 more settled bids needed of 300")));
    }

    void an_unpublished_funnel_puts_no_number_on_the_panel() {
        // No file read: one refusal line, and the scoreboard/gate cards are
        // untouched by it.
        const auto view = present_kalshi_bot_panel({}, evaluated_gate("FAIL", 4, 1.35, true), {},
                                                   9'100'000'000);
        QCOMPARE(view.funnel.size(), 1);
        QVERIFY(view.funnel.first().startsWith(QStringLiteral("FUNNEL UNAVAILABLE")));
        QVERIFY(view.scoreboard.contains(QStringLiteral("4 settled")));
    }

    void the_panel_reads_the_same_file_the_cli_publishes() {
        QTemporaryDir evidence;
        QVERIFY(evidence.isValid());
        qputenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR", evidence.path().toUtf8());
        QFile funnel(kalshi_bot_funnel_path());
        QVERIFY(funnel.open(QIODevice::WriteOnly | QIODevice::Text));
        funnel.write(QJsonDocument(QJsonObject{{"ts_ms", 9'200'000'000.0}, {"bids", 7}, {"fills", 1},
                                               {"settlements", 0}, {"fill_rate", 1.0 / 7.0}})
                         .toJson());
        funnel.close();
        const auto file = kalshi_bot_read_funnel_file(kalshi_bot_funnel_path());
        qunsetenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR");
        QVERIFY(file.available);
        QVERIFY(kalshi_bot_funnel_lines(file, 9'200'000'000)
                    .at(1)
                    .contains(QStringLiteral("1 of 7 bids")));
    }
};

QTEST_GUILESS_MAIN(KalshiBotPanelPresentationTest)
#include "tst_kalshi_bot_panel_presentation.moc"
