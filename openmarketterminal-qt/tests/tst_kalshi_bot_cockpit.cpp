// Regression tests for the BOT COCKPIT presentation model (issue #146).
//
// The scene is animated, so the thing worth defending is that every animated
// element is a DATA element: no evidence must produce no motion, a stale
// calibrator report must freeze the rain, an all-paper ledger must never reach
// the LIVE mood, and every number on the scene must be the number the CLI and
// the gate file already publish. All of that is decided by
// `present_bot_cockpit`, which is a pure function — these tests hold it to
// account without a widget.

#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "screens/kalshi/BotCockpitPresentation.h"

using namespace openmarketterminal::screens::kalshi;

namespace {

constexpr qint64 kNow = 1'785'000'000'000LL;

QJsonObject decision_row(qint64 ts_ms, const QString& ticker, const QString& reason,
                         const QString& mode = QStringLiteral("paper")) {
    QJsonObject row{{"event", "kalshi_bot_decision"},
                    {"ts_ms", double(ts_ms)},
                    {"ticker", ticker},
                    {"action", "pass"},
                    {"reason_code", reason},
                    {"signal_trusted", true}};
    if (!mode.isEmpty()) row.insert("mode", mode);
    return row;
}

QJsonObject bid_row(qint64 ts_ms, const QString& ticker, const QString& side = QStringLiteral("YES"),
                    const QString& mode = QStringLiteral("paper")) {
    QJsonObject row = decision_row(ts_ms, ticker, QStringLiteral("EDGE_CLEARS_THRESHOLD"), mode);
    row.insert("action", "bid");
    row.insert("side", side);
    row.insert("price", 0.42);
    row.insert("contracts", 4);
    row.insert("all_in_usd", 1.80);
    row.insert("edge", 0.134);
    row.insert("calibrated_p", 0.554);
    row.insert("market_mid", 0.42);
    return row;
}

QJsonObject settlement_row(qint64 ts_ms, const QString& ticker, bool won, double pnl) {
    return QJsonObject{{"event", "kalshi_bot_paper_settlement"},
                       {"ts_ms", double(ts_ms)},
                       {"mode", "paper"},
                       {"ticker", ticker},
                       {"won", won},
                       {"realized_pnl", pnl},
                       {"position_id", ticker + "@" + QString::number(ts_ms)}};
}

/// A prediction shaped exactly as calibrator.json writes one.
QJsonObject prediction(double p_yes, double mid, double sigma = 3.5) {
    return QJsonObject{
        {"p_yes_full", p_yes},
        {"p_yes_market_baseline", mid},
        {"market_yes_mid", mid},
        {"features", QJsonObject{{"required_move_sigma", sigma},
                                 {"sqrt_minutes_left", 1.4},
                                 {"signed_distance_bps", 12.0},
                                 {"yes_mid", mid}}}};
}

QJsonObject calibrator_report(qint64 generated_at_ms, const QJsonObject& predictions,
                              bool adds_value = true) {
    return QJsonObject{{"schema", 2},
                       {"event", "spot_calibrator"},
                       {"advisory_only", true},
                       {"generated_at_ms", double(generated_at_ms)},
                       {"resolved_contracts", 537},
                       {"scored_contracts", 318},
                       {"training_observations", 15'702},
                       {"brier_full", 0.0698},
                       {"brier_market_mid_raw", 0.0729},
                       {"brier_market_trained_logit", 0.0741},
                       {"adds_value_over_market", adds_value},
                       {"predictions", predictions}};
}

QJsonObject criterion(const QString& id, double observed, double required,
                      const QString& comparison, bool met) {
    return QJsonObject{{"id", id},
                       {"met", met},
                       {"observed", observed},
                       {"required", required},
                       {"comparison", comparison}};
}

/// A verdict shaped exactly like KalshiBotGate::evaluate() writes it, including
/// the `ledger` block the KPI strip reads verbatim.
QJsonObject evaluated_gate(const QString& verdict, int settled, int wins, int losses,
                           double net_pnl, double drawdown, qint64 ts_ms = kNow - 60'000) {
    QJsonObject brier{{"id", "brier_beats_market"},
                      {"met", true},
                      {"brier_available", true},
                      {"scored_contracts", settled},
                      {"observed", 0.2240},
                      {"brier_bot", 0.2240},
                      {"brier_market_baseline", 0.2425},
                      {"brier_margin", 0.0},
                      {"required", 0.2425},
                      {"comparison", "brier_bot < brier_market_baseline - brier_margin"}};
    return QJsonObject{
        {"schema", 1},
        {"event", "kalshi_bot_gate"},
        {"verdict", verdict},
        {"evaluated", true},
        {"ts_ms", double(ts_ms)},
        {"mode", "paper"},
        {"params", QJsonObject{{"min_settled_bids", 300},
                               {"min_net_pnl_usd", 0},
                               {"min_brier_margin", 0},
                               {"max_drawdown_usd", 20}}},
        {"criteria",
         QJsonArray{criterion("min_settled_bids", settled, 300, "observed >= required",
                              settled >= 300),
                    criterion("net_pnl_usd", net_pnl, 0.0, "observed > required", net_pnl > 0.0),
                    brier,
                    criterion("max_drawdown_usd", drawdown, 20.0, "observed <= required",
                              drawdown <= 20.0)}},
        {"ledger", QJsonObject{{"settled_bids", settled},
                               {"wins", wins},
                               {"losses", losses},
                               {"net_pnl_usd", net_pnl},
                               {"fees_usd", 0.17},
                               {"stake_usd", 6.48},
                               {"max_drawdown_usd", drawdown},
                               {"scored_contracts", settled},
                               {"unscored_contracts", 0}}}};
}

/// An armed live session as `kalshi auto live status` reports one.
QJsonObject armed_status() {
    return QJsonObject{{"session_active", true},
                       {"per_bet_contract_stake_cap", 2.0},
                       {"per_bet_all_in_tolerance", 3.0},
                       {"max_orders_per_hour", 6},
                       {"worst_case_exposure_used", 4.5},
                       {"experiment_cap", 120.0},
                       {"kill_switch", false}};
}

QJsonObject disarmed_status() { return QJsonObject{{"session_active", false}}; }

/// The panel view the cockpit is built on — always produced by the panel's own
/// presenter, never hand-assembled, so a cockpit test cannot pass against a
/// state the panel would not actually report.
KalshiBotPanelView panel_for(const QJsonArray& ledger, const QJsonObject& gate = {},
                             const QJsonObject& live_status = {}, qint64 now_ms = kNow,
                             const openmarketterminal::services::prediction::kalshi_ns::
                                 KalshiBotStopFile& stop = {}) {
    return present_kalshi_bot_panel(ledger, gate, live_status, now_ms, 8, stop);
}

QStringList pulse_keys(const BotCockpitScene& scene, const QString& kind) {
    QStringList keys;
    for (const auto& pulse : scene.pulses)
        if (pulse.kind == kind) keys << pulse.key;
    return keys;
}

} // namespace

class KalshiBotCockpitTest : public QObject {
    Q_OBJECT

  private slots:

    // ── criterion 1a: no data -> no motion ─────────────────────────────────

    void no_evidence_produces_no_motion_and_no_rain() {
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for({}), {}, {}, {}, {}, kNow);
        QVERIFY(!scene.motion);
        QVERIFY(scene.columns.isEmpty());
        QVERIFY(scene.pulses.isEmpty());
        QCOMPARE(scene.columns_total, 0);
        QVERIFY(scene.dormant);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodDormant));
        // The absence is stated, not left as an empty scene the viewer must
        // interpret.
        QVERIFY(scene.census.contains(QStringLiteral("NO RAIN")));
        QVERIFY(scene.census.contains(QStringLiteral("calibrator.json")));
        QVERIFY(!scene.report_present);
        QVERIFY(scene.envelope.contains(QStringLiteral("NO DECISION JOURNALED")));
    }

    void a_running_loop_with_a_fresh_report_moves() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report = calibrator_report(
            kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QVERIFY(scene.motion);
        QCOMPARE(scene.columns.size(), 1);
        QVERIFY(!scene.columns.first().frozen);
        QCOMPARE(scene.columns_frozen, 0);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodPaper));
    }

    // A report the bot would still trade off, but a loop that has stopped
    // ticking: the cockpit is dead even though the rain data is fresh.
    void a_dormant_loop_never_moves_even_on_a_fresh_report() {
        const QJsonArray ledger{decision_row(kNow - 600'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report = calibrator_report(
            kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QCOMPARE(panel_for(ledger).state, QStringLiteral("stale"));
        QVERIFY(scene.dormant);
        QVERIFY(!scene.motion);
        QVERIFY(!scene.columns.first().frozen);  // the report itself is fine
    }

    // ── criterion 1b: stale data -> frozen columns ─────────────────────────

    void a_stale_report_freezes_every_column_and_stops_motion() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report =
            calibrator_report(kNow - 300'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)},
                                                          {"KX-B", prediction(0.21, 0.30)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QCOMPARE(scene.columns.size(), 2);
        for (const auto& column : scene.columns) {
            QVERIFY(column.frozen);
            QVERIFY(!column.frozen_reason.isEmpty());
        }
        QCOMPARE(scene.columns_frozen, 2);
        QVERIFY(!scene.motion);
        QVERIFY(scene.census.contains(QStringLiteral("FROZEN")));
    }

    void a_report_dated_in_the_future_freezes_rather_than_reads_fresh() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report =
            calibrator_report(kNow + 60'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QVERIFY(scene.columns.first().frozen);
        QVERIFY(scene.columns.first().frozen_reason.contains(QStringLiteral("clock skew")));
        QVERIFY(!scene.motion);
    }

    // The freeze bound is the bot's own report-refusal age, not one this scene
    // invented. If KalshiBotDecision ever changes it, the rain follows.
    void the_freeze_bound_is_the_bots_own_report_refusal_age() {
        using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
        QCOMPARE(bot_cockpit_freeze_bound_ms(), KalshiBotDecision::Config{}.max_report_age_ms);

        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const qint64 bound = bot_cockpit_freeze_bound_ms();
        const auto frozen_at = [&](qint64 age) {
            const QJsonObject report =
                calibrator_report(kNow - age, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
            return present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow)
                .columns.first()
                .frozen;
        };
        QVERIFY(!frozen_at(bound - 1'000));
        QVERIFY(frozen_at(bound + 1'000));
    }

    // ── criterion 1c: an all-paper ledger can never render the LIVE mood ───

    void an_all_paper_ledger_never_renders_the_live_mood() {
        QJsonArray ledger;
        for (int i = 0; i < 6; ++i)
            ledger.append(decision_row(kNow - (i * 1'000), QStringLiteral("KX-A"),
                                       QStringLiteral("EDGE_BELOW_THRESHOLD")));
        ledger.append(bid_row(kNow - 2'000, QStringLiteral("KX-B")));
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QVERIFY(!scene.live);
        QVERIFY(scene.mood != QString::fromLatin1(kBotCockpitMoodLive));
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodPaper));
        QVERIFY(scene.banner.startsWith(QStringLiteral("PAPER")));
    }

    // A live tick an hour ago followed by paper ticks is papering NOW.
    void an_older_live_row_does_not_latch_the_live_mood() {
        const QJsonArray ledger{
            bid_row(kNow - 3'600'000, QStringLiteral("KX-A"), QStringLiteral("YES"),
                    QStringLiteral("live")),
            decision_row(kNow - 5'000, QStringLiteral("KX-B"),
                         QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        QVERIFY(!scene.live);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodPaper));
    }

    void a_live_newest_tick_renders_the_live_mood_and_banner() {
        const QJsonArray ledger{bid_row(kNow - 3'000, QStringLiteral("KX-A"),
                                        QStringLiteral("YES"), QStringLiteral("live"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        QVERIFY(scene.live);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodLive));
        QVERIFY(scene.banner.contains(QStringLiteral("REAL MONEY")));
        QVERIFY(!scene.dormant);
    }

    // The advisory strategy-grid line is plumbed from the verdict bytes and fails
    // closed to UNAVAILABLE when there are none. (The line's full formatting is
    // held to account by tst_kalshi_strategy_grid_view.)
    void the_grid_line_surfaces_the_verdict_and_fails_closed() {
        const QByteArray no_edge = QByteArrayLiteral(
            "{\"schema_version\":1,\"as_of_utc\":\"2026-07-30T00:00:00+00:00\","
            "\"headline\":\"no variant beats hold+market after correction\","
            "\"survivors\":[],\"candidates\":[]}");
        const BotCockpitScene edge_scene =
            present_bot_cockpit(panel_for({}), {}, {}, {}, {}, kNow, no_edge);
        QCOMPARE(edge_scene.grid_line, QStringLiteral("GRID: no measured edge"));
        const BotCockpitScene missing =
            present_bot_cockpit(panel_for({}), {}, {}, {}, {}, kNow, QByteArray());
        QCOMPARE(missing.grid_line, QStringLiteral("GRID: UNAVAILABLE"));
    }

    // A ledger row this build cannot read fails CLOSED (issue #145): the mood
    // must be dormant, never live and never a confident paper.
    void an_unreadable_newest_row_renders_dormant_not_live() {
        QJsonArray ledger{decision_row(kNow - 9'000, QStringLiteral("KX-A"),
                                       QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        ledger.append(decision_row(kNow - 3'000, QStringLiteral("KX-B"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD"), QString()));
        const KalshiBotPanelView panel = panel_for(ledger);
        QVERIFY(panel.mode_unknown);
        const BotCockpitScene scene = present_bot_cockpit(panel, {}, {}, ledger, {}, kNow);
        QVERIFY(!scene.live);
        QVERIFY(scene.dormant);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodDormant));
        QVERIFY(scene.mood_reason.contains(QStringLiteral("no mode this build can read")));
    }

    // A stopped loop that has live orders out is NOT a dead cockpit: real money
    // outranks the loop state on the mood, exactly as it does on the BOT chip.
    void a_live_tick_outranks_a_stopped_loop_on_the_mood() {
        openmarketterminal::services::prediction::kalshi_ns::KalshiBotStopFile stop;
        stop.engaged = true;
        stop.ts_ms = kNow - 1'000;
        const QJsonArray ledger{bid_row(kNow - 3'000, QStringLiteral("KX-A"),
                                        QStringLiteral("YES"), QStringLiteral("live"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, {}, {}, kNow, stop), {}, {}, ledger, {}, kNow);
        QCOMPARE(scene.mood, QString::fromLatin1(kBotCockpitMoodLive));
        const BotCockpitNode* kill = scene.node(QStringLiteral("kill_switch"));
        QVERIFY(kill != nullptr);
        QVERIFY(kill->value.contains(QStringLiteral("ENGAGED")));
        QCOMPARE(kill->role, QStringLiteral("red"));
    }

    // ── criterion 1d: one ignition per real journal row ────────────────────

    void two_bids_on_one_contract_are_two_ignitions() {
        const QJsonArray ledger{bid_row(kNow - 120'000, QStringLiteral("KX-A")),
                                bid_row(kNow - 5'000, QStringLiteral("KX-A"),
                                        QStringLiteral("NO"))};
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QCOMPARE(scene.columns.first().ignitions, 2);
        const QStringList keys = pulse_keys(scene, QStringLiteral("ignition"));
        QCOMPARE(keys.size(), 2);
        // Keyed by (ts_ms, ticker): two rows, two distinct keys.
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()).size(), 2);
        // The column shows the NEWEST bid's side, so a re-quote onto the other
        // side is not rendered as the old one.
        QCOMPARE(scene.columns.first().ignition_side, QStringLiteral("NO"));
    }

    void the_same_ledger_presented_twice_produces_the_same_event_keys() {
        const QJsonArray ledger{bid_row(kNow - 120'000, QStringLiteral("KX-A")),
                                bid_row(kNow - 5'000, QStringLiteral("KX-B")),
                                settlement_row(kNow - 60'000, QStringLiteral("KX-C"), true, 0.35)};
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55);
        const BotCockpitScene first =
            present_bot_cockpit(panel_for(ledger, gate), report, gate, ledger, {}, kNow);
        const BotCockpitScene second =
            present_bot_cockpit(panel_for(ledger, gate), report, gate, ledger, {}, kNow);
        QStringList a;
        QStringList b;
        for (const auto& pulse : first.pulses) a << pulse.key;
        for (const auto& pulse : second.pulses) b << pulse.key;
        QCOMPARE(a, b);
        QCOMPARE(first.envelope_key, second.envelope_key);
        // Two ignitions, one dissolve, one calibrator refresh, one gate verdict.
        QCOMPARE(pulse_keys(first, QStringLiteral("ignition")).size(), 2);
        QCOMPARE(pulse_keys(first, QStringLiteral("dissolve")).size(), 1);
        QCOMPARE(pulse_keys(first, QStringLiteral("calibrator")).size(), 1);
        QCOMPARE(pulse_keys(first, QStringLiteral("gate")).size(), 1);
    }

    void a_settled_contract_dissolves_with_its_real_pnl() {
        const QJsonArray ledger{settlement_row(kNow - 30'000, QStringLiteral("KX-A"), true, 0.35),
                                settlement_row(kNow - 20'000, QStringLiteral("KX-B"), false,
                                               -1.55)};
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.90, 0.88)},
                                                          {"KX-B", prediction(0.10, 0.12)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        for (const auto& column : scene.columns) {
            QVERIFY(column.settled);
            if (column.ticker == QStringLiteral("KX-A")) {
                QVERIFY(column.settled_won);
                QCOMPARE(column.settled_pnl_usd, 0.35);
            } else {
                QVERIFY(!column.settled_won);
                QCOMPARE(column.settled_pnl_usd, -1.55);
            }
        }
        const QStringList keys = pulse_keys(scene, QStringLiteral("dissolve"));
        QCOMPARE(keys.size(), 2);
    }

    // ── criterion 3: the numbers are the CLI's and the gate file's ─────────

    // The edge glyph must be the same subtraction the bot journals, so a
    // contract's rain and its ledger row cannot disagree.
    void the_edge_glyph_equals_the_ledgers_own_edge_for_the_same_numbers() {
        const double p = 0.024386349758544628;
        const double mid = 0.025000000000000015;
        QJsonObject row = decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                       QStringLiteral("EDGE_BELOW_THRESHOLD"));
        row.insert("calibrated_p", p);
        row.insert("market_mid", mid);
        row.insert("edge", p - mid);
        const QJsonArray ledger{row};
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(p, mid)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        const BotCockpitColumn& column = scene.columns.first();
        bool found = false;
        for (const auto& glyph : column.glyphs) {
            if (glyph.label != QStringLiteral("edge")) continue;
            found = true;
            QVERIFY(glyph.known);
            QCOMPARE(glyph.value, row.value(QStringLiteral("edge")).toDouble());
        }
        QVERIFY(found);
    }

    void the_kpi_strip_is_the_gates_own_ledger_block_verbatim() {
        const QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55);
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
        QVERIFY(scene.kpi_available);
        const QString strip = scene.kpi.join(QStringLiteral(" | "));
        QVERIFY(strip.contains(QStringLiteral("SETTLED 4")));
        QVERIFY(strip.contains(QStringLiteral("W/L 3-1")));
        QVERIFY(strip.contains(QStringLiteral("NET $1.35")));
        // Drawdown is shown against the SEALED cap, not against a number the
        // scene picked.
        QVERIFY(strip.contains(QStringLiteral("DRAWDOWN $1.55 / $20.00")));
        // Every entry carries its own colour role, decided here rather than
        // re-derived by the widget from the rendered text.
        QCOMPARE(scene.kpi_roles.size(), scene.kpi.size());
    }

    // The one strip entry the issue asks to be coloured by sign — and the one
    // that must go red when it breaches the sealed cap.
    void the_net_pnl_and_drawdown_entries_carry_their_own_colour_roles() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const auto role_for = [&](const QJsonObject& gate, const QString& prefix) {
            const BotCockpitScene scene =
                present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
            for (int i = 0; i < scene.kpi.size(); ++i)
                if (scene.kpi.at(i).startsWith(prefix)) return scene.kpi_roles.value(i);
            return QStringLiteral("(no such entry)");
        };
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55),
                          QStringLiteral("NET")),
                 QStringLiteral("green"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 1, 3, -2.10, 3.00),
                          QStringLiteral("NET")),
                 QStringLiteral("red"));
        // A flat book is neither a win nor a loss and is not painted as one.
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 2, 2, 0.0, 1.00),
                          QStringLiteral("NET")),
                 QStringLiteral("grey"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55),
                          QStringLiteral("DRAWDOWN")),
                 QStringLiteral("grey"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 1, 3, -2.10, 25.00),
                          QStringLiteral("DRAWDOWN")),
                 QStringLiteral("red"));
    }

    // No gate file means no scoreboard — never a strip of zeroes.
    void an_absent_gate_reads_unavailable_rather_than_zero() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        QVERIFY(!scene.kpi_available);
        const QString strip = scene.kpi.join(QStringLiteral(" | "));
        QVERIFY(strip.contains(QStringLiteral("SCOREBOARD UNAVAILABLE")));
        QVERIFY(!strip.contains(QStringLiteral("W/L 0-0")));
        QVERIFY(!strip.contains(QStringLiteral("$0.00")));
        const BotCockpitNode* settlements = scene.node(QStringLiteral("settlements"));
        QVERIFY(settlements != nullptr);
        QVERIFY(!settlements->known);
        QVERIFY(settlements->value.contains(QStringLiteral("UNAVAILABLE")));
    }

    // A gate that refused to evaluate publishes no ledger block; the strip must
    // report the refusal, not silently drop to zeroes.
    void an_unevaluated_gate_reads_unavailable() {
        const QJsonObject gate{{"event", "kalshi_bot_gate"},
                               {"evaluated", false},
                               {"verdict", "NOT_PREREGISTERED"},
                               {"reason", "no params file"},
                               {"ts_ms", double(kNow - 30'000)}};
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
        QVERIFY(!scene.kpi_available);
        QVERIFY(scene.kpi_unavailable_reason.contains(QStringLiteral("NOT_PREREGISTERED")));
    }

    // The gate node prints the criteria through the BOT panel's own formatter,
    // so the two surfaces cannot word a criterion differently.
    void the_gate_node_prints_the_panels_own_criterion_lines() {
        const QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55);
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("gate"));
        QVERIFY(node != nullptr);
        QVERIFY(node->value.startsWith(QStringLiteral("FAIL · evaluated 60s ago · 3/4 criteria "
                                                      "met")));
        for (const auto& value : gate.value(QStringLiteral("criteria")).toArray())
            QVERIFY(node->value.contains(kalshi_bot_detail::criterion_line(value.toObject())));
    }

    // --- the verdict's age in the cockpit (issue #167) ----------------------

    // The gate node states when the verdict was evaluated, right after the
    // verdict word, from the panel's own reading — one age, two surfaces.
    void the_gate_node_states_the_evaluations_age() {
        const QJsonObject gate = evaluated_gate(QStringLiteral("PASS"), 340, 200, 140, 1.35, 1.55);
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const KalshiBotPanelView panel = panel_for(ledger, gate);
        const BotCockpitScene scene =
            present_bot_cockpit(panel, {}, gate, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("gate"));
        QVERIFY(node != nullptr);
        QVERIFY2(node->value.contains(panel.gate_age), qPrintable(node->value));
        QCOMPARE(node->role, QStringLiteral("green"));
    }

    // A PASS the loop has not re-evaluated inside the staleness window renders
    // visibly stale — amber node, amber pulse, and the age in words. It must
    // never sit on the cockpit looking like a current promotion.
    void a_stale_pass_renders_amber_in_the_cockpit_not_green() {
        const QJsonObject gate = evaluated_gate(QStringLiteral("PASS"), 340, 200, 140, 1.35, 1.55,
                                                kNow - 5LL * 3'600'000);
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const KalshiBotPanelView panel = panel_for(ledger, gate);
        QVERIFY(panel.gate_pass);
        const BotCockpitScene scene = present_bot_cockpit(panel, {}, gate, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("gate"));
        QVERIFY(node != nullptr);
        QCOMPARE(node->role, QStringLiteral("amber"));
        QVERIFY2(node->value.contains(QStringLiteral("evaluated 5h ago")), qPrintable(node->value));
        QVERIFY2(node->value.contains(QStringLiteral("STALE")), qPrintable(node->value));
        for (const BotCockpitPulse& pulse : scene.pulses)
            if (pulse.kind == QStringLiteral("gate")) {
                QCOMPARE(pulse.role, QStringLiteral("amber"));
                QVERIFY2(pulse.text.contains(QStringLiteral("evaluated 5h ago")),
                         qPrintable(pulse.text));
            }
    }

    void the_exposure_node_quotes_the_sessions_caps_or_states_their_absence() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene armed = present_bot_cockpit(
            panel_for(ledger, {}, armed_status()), {}, {}, ledger, armed_status(), kNow);
        const BotCockpitNode* node = armed.node(QStringLiteral("exposure"));
        QVERIFY(node != nullptr);
        QCOMPARE(node->value,
                 QStringLiteral("$4.50 of $120.00 · stake <= $2.00 · all-in <= $3.00"));

        const BotCockpitScene disarmed = present_bot_cockpit(
            panel_for(ledger, {}, disarmed_status()), {}, {}, ledger, disarmed_status(), kNow);
        QVERIFY(disarmed.node(QStringLiteral("exposure"))
                    ->value.contains(QStringLiteral("DISARMED")));

        // An unreadable session is never rendered as disarmed.
        const BotCockpitScene unknown =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        const BotCockpitNode* unknown_node = unknown.node(QStringLiteral("exposure"));
        QVERIFY(!unknown_node->known);
        QVERIFY(unknown_node->value.contains(QStringLiteral("FAIL CLOSED")));
    }

    // A cap that the session did not state is unknown, not a default ceiling.
    void a_cap_the_session_did_not_state_reads_unknown() {
        QJsonObject status = armed_status();
        status.remove(QStringLiteral("experiment_cap"));
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, {}, status), {}, {}, ledger, status, kNow);
        QVERIFY(scene.node(QStringLiteral("exposure"))
                    ->value.contains(QStringLiteral("unknown")));
    }

    void the_calibrator_node_reports_the_reports_own_track_record() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("calibrator"));
        QVERIFY(node != nullptr);
        QVERIFY(node->known);
        // Whose track record it is, said on the label: the gate's
        // brier_beats_market criterion is a different measure over different
        // contracts and sits three boxes away on the same scene.
        QCOMPARE(node->label, QStringLiteral("CALIBRATOR — ITS OWN TRACK RECORD"));
        // Issue #171: the opponent is named (the RAW MID, not the gate's own
        // settled bids), and the count beside the score is the Brier's actual
        // denominator. `resolved_contracts` is a lifetime total; printing it
        // here read as 537 contracts of evidence behind a score computed over
        // a fraction of that.
        QVERIFY(node->value.contains(QStringLiteral("Brier 0.0698 vs raw mid 0.0729")));
        QVERIFY(node->value.contains(QStringLiteral("318 scored contracts")));
        QVERIFY(!node->value.contains(QStringLiteral("537")));
        QVERIFY(node->value.contains(QStringLiteral("ADDS VALUE")));

        // A report with no track record is not scored as zero.
        QJsonObject unscored = report;
        unscored.remove(QStringLiteral("brier_full"));
        const BotCockpitScene bare =
            present_bot_cockpit(panel_for(ledger), unscored, {}, ledger, {}, kNow);
        QVERIFY(!bare.node(QStringLiteral("calibrator"))->known);
        QVERIFY(bare.node(QStringLiteral("calibrator"))
                    ->value.contains(QStringLiteral("NOT SCORED")));
    }

    // ── missing numbers read missing, never zero ───────────────────────────

    void a_prediction_without_a_mid_renders_missing_glyphs() {
        QJsonObject bare = prediction(0.55, 0.42);
        bare.remove(QStringLiteral("market_yes_mid"));
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report = calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", bare}});
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        const BotCockpitColumn& column = scene.columns.first();
        QVERIFY(!column.edge_known);
        for (const auto& glyph : column.glyphs) {
            if (glyph.label == QStringLiteral("mid") || glyph.label == QStringLiteral("edge")) {
                QVERIFY(!glyph.known);
                QCOMPARE(glyph.text, QString::fromUtf8("—"));
            } else {
                QVERIFY(glyph.known);
            }
        }
    }

    // ── the cap on drawn columns is never silent ───────────────────────────

    void a_capped_column_set_says_how_many_it_did_not_draw() {
        QJsonObject predictions;
        for (int i = 0; i < 40; ++i)
            predictions.insert(QStringLiteral("KX-%1").arg(i, 2, 10, QLatin1Char('0')),
                               prediction(0.50 + (i * 0.005), 0.42));
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-00"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report = calibrator_report(kNow - 10'000, predictions);
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                QByteArray(), 5);
        QCOMPARE(scene.columns.size(), 5);
        QCOMPARE(scene.columns_total, 40);
        QVERIFY(scene.census.contains(QStringLiteral("5 of 40 watched contracts")));
        QVERIFY(scene.census.contains(QStringLiteral("35 not drawn")));
        // Ranked by |edge|: the widest-edge contract is drawn first.
        QVERIFY(scene.columns.first().abs_edge >= scene.columns.last().abs_edge);
    }

    // ── the centre envelope is the newest real decision ────────────────────

    void the_envelope_flashes_the_newest_decision_text() {
        const QJsonArray passes{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        QCOMPARE(present_bot_cockpit(panel_for(passes), {}, {}, passes, {}, kNow).envelope,
                 QStringLiteral("PASS · EDGE_BELOW_THRESHOLD"));

        const QJsonArray bids{decision_row(kNow - 9'000, QStringLiteral("KX-A"),
                                           QStringLiteral("EDGE_BELOW_THRESHOLD")),
                              bid_row(kNow - 3'000, QStringLiteral("KX-B"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(bids), {}, {}, bids, {}, kNow);
        QCOMPARE(scene.envelope, QStringLiteral("BID YES $0.42 x4"));
        QCOMPARE(scene.envelope_ticker, QStringLiteral("KX-B"));
        QCOMPARE(scene.envelope_role, QStringLiteral("green"));

        const QJsonArray live{bid_row(kNow - 3'000, QStringLiteral("KX-B"),
                                      QStringLiteral("NO"), QStringLiteral("live"))};
        const BotCockpitScene live_scene =
            present_bot_cockpit(panel_for(live), {}, {}, live, {}, kNow);
        QVERIFY(live_scene.envelope.startsWith(QStringLiteral("LIVE · BID NO")));
        QCOMPARE(live_scene.envelope_role, QStringLiteral("red"));
    }

    /// Issue #158: the envelope is the one line a passing glance catches, so
    /// it has to say whether the bot PAID for that fill or is still waiting.
    /// A row from a build that quoted only one way claims neither.
    void the_envelope_says_whether_the_bid_crossed_or_rested() {
        QJsonObject crossed = bid_row(kNow - 3'000, QStringLiteral("KX-B"));
        crossed.insert(QStringLiteral("quote_style"), QStringLiteral("cross"));
        const QJsonArray crossing{crossed};
        QCOMPARE(present_bot_cockpit(panel_for(crossing), {}, {}, crossing, {}, kNow).envelope,
                 QStringLiteral("BID YES $0.42 x4 · CROSS"));

        QJsonObject rested = bid_row(kNow - 3'000, QStringLiteral("KX-B"));
        rested.insert(QStringLiteral("quote_style"), QStringLiteral("rest"));
        const QJsonArray resting{rested};
        QCOMPARE(present_bot_cockpit(panel_for(resting), {}, {}, resting, {}, kNow).envelope,
                 QStringLiteral("BID YES $0.42 x4 · REST"));

        const QJsonArray silent{bid_row(kNow - 3'000, QStringLiteral("KX-B"))};
        QCOMPARE(present_bot_cockpit(panel_for(silent), {}, {}, silent, {}, kNow).envelope,
                 QStringLiteral("BID YES $0.42 x4"));
    }

    // ── criterion 2: the BOT tab suggests the cockpit while running ────────

    void the_cockpit_is_suggested_exactly_while_the_loop_is_running() {
        const QJsonArray running{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                              QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        QVERIFY(present_bot_cockpit(panel_for(running), {}, {}, running, {}, kNow)
                    .suggest_cockpit);

        const QJsonArray stale{decision_row(kNow - 600'000, QStringLiteral("KX-A"),
                                            QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        QVERIFY(!present_bot_cockpit(panel_for(stale), {}, {}, stale, {}, kNow).suggest_cockpit);

        QVERIFY(!present_bot_cockpit(panel_for({}), {}, {}, {}, {}, kNow).suggest_cockpit);
    }

    // ── the decision rate states its own window ────────────────────────────

    // Every tick writes one row PER CONTRACT and the ledger tail is a byte
    // window, not an hour: the rate must be counted, not divided, and must say
    // what it counted over.
    void the_decision_rate_counts_the_last_hour_and_states_the_window() {
        QJsonArray ledger;
        for (int i = 0; i < 3; ++i)
            ledger.append(decision_row(kNow - 10'000, QStringLiteral("KX-%1").arg(i),
                                       QStringLiteral("EDGE_BELOW_THRESHOLD")));
        for (int i = 0; i < 2; ++i)
            ledger.append(decision_row(kNow - 70'000, QStringLiteral("KX-%1").arg(i),
                                       QStringLiteral("EDGE_BELOW_THRESHOLD")));
        // Older than an hour: counted by nobody.
        ledger.append(decision_row(kNow - 4'000'000, QStringLiteral("KX-OLD"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD")));
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        const QString strip = scene.kpi.join(QStringLiteral(" | "));
        QVERIFY(strip.contains(QStringLiteral("5 decisions / 2 ticks in the last 60m")));
        // A SPAN, not an age: "spans 66m", never "spans 66m ago".
        QVERIFY(strip.contains(QStringLiteral("ledger window spans 66m)")));
    }
};

QTEST_GUILESS_MAIN(KalshiBotCockpitTest)
#include "tst_kalshi_bot_cockpit.moc"
