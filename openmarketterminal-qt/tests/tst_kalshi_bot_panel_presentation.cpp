#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

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
};

QTEST_GUILESS_MAIN(KalshiBotPanelPresentationTest)
#include "tst_kalshi_bot_panel_presentation.moc"
