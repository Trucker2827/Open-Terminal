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

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "screens/kalshi/BotCockpitFeedHealthReader.h"
#include "screens/kalshi/BotCockpitPresentation.h"
#include "screens/kalshi/BotCockpitRainLabels.h"

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

/// A prediction shaped exactly as calibrator.json writes one (threshold).
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

/// KXBTC15M directional prediction — carries open_price so rain falls `open`
/// bps rather than strike-σ.
QJsonObject directional_prediction(double p_yes, double mid, double open_bps = 12.0) {
    return QJsonObject{
        {"p_yes_full", p_yes},
        {"p_yes_market_baseline", mid},
        {"market_yes_mid", mid},
        {"features", QJsonObject{{"open_price", 64000.0},
                                 {"signed_distance_bps", open_bps},
                                 {"per_min_vol_bps", 4.0},
                                 {"seconds_left", 400.0},
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
                       {"adds_value_on_bet_eligible", adds_value},
                       {"brier_eligible_full", adds_value ? 0.2130 : 0.2576},
                       {"brier_eligible_market_mid_raw", adds_value ? 0.2576 : 0.2130},
                       {"eligible_scored_contracts", 104},
                       {"min_eligible_contracts", 100},
                       {"predictions", predictions}};
}

/// Shaped like kxbtc15m-calibrator.json — own Brier, own floor, own trust.
QJsonObject kxbtc15m_calibrator_report(qint64 generated_at_ms, const QJsonObject& predictions,
                                       bool adds_value = false, int scored = 7,
                                       double brier_full = 0.2100,
                                       double brier_mid = 0.2400,
                                       const QString& trusted_variant = {},
                                       const QJsonObject& ablations = {}) {
    QJsonObject out{{"schema", 4},
                    {"event", "kxbtc15m_calibrator"},
                    {"advisory_only", true},
                    {"generated_at_ms", double(generated_at_ms)},
                    {"resolved_contracts", scored},
                    {"scored_contracts", scored},
                    {"min_scored_contracts", 100},
                    {"brier_full", brier_full},
                    {"brier_market_mid_raw", brier_mid},
                    {"adds_value_over_market", adds_value},
                    {"adds_value_on_bet_eligible", adds_value},
                    {"brier_eligible_full", adds_value ? 0.2130 : 0.2576},
                    {"brier_eligible_market_mid_raw", adds_value ? 0.2576 : 0.2130},
                    {"eligible_scored_contracts", 104},
                    {"min_eligible_contracts", 100},
                    {"predictions", predictions}};
    if (!trusted_variant.isEmpty())
        out.insert(QStringLiteral("trusted_variant"), trusted_variant);
    if (!ablations.isEmpty()) out.insert(QStringLiteral("ablations"), ablations);
    return out;
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

// ── health-first classifier fixtures (Task 1) ──────────────────────────────

/// A single prediction, trusted or not is decided by `calibrator_report`'s own
/// `adds_value` argument — this fixture only supplies the report's rain.
QJsonObject one_trusted_prediction() {
    return QJsonObject{{"KX-A", prediction(0.55, 0.42)}};
}

/// A ticking loop (newest row well inside the staleness window) with a recent
/// bid, so DECIDE has something to call "bidding".
QJsonArray bidding_ledger() {
    return QJsonArray{decision_row(kNow - 65'000, QStringLiteral("KX-A"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD")),
                      bid_row(kNow - 5'000, QStringLiteral("KX-B"))};
}

/// A ticking loop that is only passing — no bid in the window — so DECIDE
/// reads "passing", not "bidding".
QJsonArray passing_ledger() {
    return QJsonArray{decision_row(kNow - 60'000, QStringLiteral("KX-A"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD")),
                      decision_row(kNow - 5'000, QStringLiteral("KX-B"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD"))};
}

/// A ticking loop whose recent decisions are dominated by REPORT_STALE — the
/// calibrator's fault, not the loop's. DECIDE must not redden on this.
QJsonArray report_stale_ledger() {
    return QJsonArray{decision_row(kNow - 65'000, QStringLiteral("KX-A"),
                                   QStringLiteral("REPORT_STALE")),
                      decision_row(kNow - 5'000, QStringLiteral("KX-B"),
                                   QStringLiteral("REPORT_STALE"))};
}

/// A ticking loop whose ONLY bid is older than `kDecideBidRecencyMs`, with a
/// recent PASS after it so the loop still reads "running". The ledger tail is
/// a byte window, not a time window, so this old bid can sit in it for a long
/// stretch after the loop stopped bidding — DECIDE must not still call that
/// "bidding".
QJsonArray old_bid_ledger() {
    return QJsonArray{bid_row(kNow - 200'000, QStringLiteral("KX-A")),
                      decision_row(kNow - 5'000, QStringLiteral("KX-B"),
                                   QStringLiteral("EDGE_BELOW_THRESHOLD"))};
}

// Feed-health fixtures.
BotCockpitFeedHealth feed_live() { return {true, true, true, 2'000, {}}; }
BotCockpitFeedHealth feed_down() {
    return {true, false, true, 14'400'000, QStringLiteral("WebSocket disconnected")};
}

QString stage_role(const BotCockpitScene& s, const QString& id) {
    for (const auto& st : s.health_stages)
        if (st.id == id) return st.role;
    return QStringLiteral("missing");
}

QString stage_value(const BotCockpitScene& s, const QString& id) {
    for (const auto& st : s.health_stages)
        if (st.id == id) return st.value;
    return QStringLiteral("missing");
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
        QVERIFY(scene.census.contains(QStringLiteral("NO FLOW")));
        QVERIFY(scene.census.contains(QStringLiteral("multi-cadence calibrators")));
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
        // Exact refuse age must freeze — matches KalshiBotDecision age_ms >= bound.
        QVERIFY(frozen_at(bound));
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
        // No current-rules postmortem → HIST tag on gate ledger numbers.
        QVERIFY(strip.contains(QStringLiteral("SETTLED HIST 4")));
        QVERIFY(strip.contains(QStringLiteral("W/L 3-1")));
        QVERIFY(strip.contains(QStringLiteral("NET HIST $1.35")));
        // Lifetime drawdown vs sealed cap (promotion fact, not new-rules P&L).
        QVERIFY(strip.contains(QStringLiteral("HIST DD $1.55 / $20.00")));
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
                          QStringLiteral("NET HIST")),
                 QStringLiteral("green"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 1, 3, -2.10, 3.00),
                          QStringLiteral("NET HIST")),
                 QStringLiteral("red"));
        // A flat book is neither a win nor a loss and is not painted as one.
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 2, 2, 0.0, 1.00),
                          QStringLiteral("NET HIST")),
                 QStringLiteral("grey"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55),
                          QStringLiteral("HIST DD")),
                 QStringLiteral("grey"));
        QCOMPARE(role_for(evaluated_gate(QStringLiteral("FAIL"), 4, 1, 3, -2.10, 25.00),
                          QStringLiteral("HIST DD")),
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
        QCOMPARE(node->label, QStringLiteral("BTC 1H — THRESHOLD SCOREBOARD · click"));
        // Issue #171: the opponent is named (the RAW MID, not the gate's own
        // settled bids), and the count beside the score is the Brier's actual
        // denominator. `resolved_contracts` is a lifetime total; printing it
        // here read as 537 contracts of evidence behind a score computed over
        // a fraction of that.
        QVERIFY(node->value.contains(QStringLiteral("Brier 0.0698 vs raw mid 0.0729")));
        QVERIFY(node->value.contains(QStringLiteral("318 scored contracts")));
        QVERIFY(!node->value.contains(QStringLiteral("537")));
        QVERIFY(node->value.contains(QStringLiteral("ADDS VALUE")));
        QVERIFY(!node->detail.isEmpty());

        // A report with no track record is not scored as zero.
        QJsonObject unscored = report;
        unscored.remove(QStringLiteral("brier_full"));
        const BotCockpitScene bare =
            present_bot_cockpit(panel_for(ledger), unscored, {}, ledger, {}, kNow);
        QVERIFY(!bare.node(QStringLiteral("calibrator"))->known);
        QVERIFY(bare.node(QStringLiteral("calibrator"))
                    ->value.contains(QStringLiteral("NOT SCORED")));
    }

    // Fix round 1, Finding 2: the panel and the bot must never disagree about
    // promotion (KalshiBotCommands.cpp: "one scorer, many readers"). A report
    // that beats the mid over the full population but loses where the bot
    // actually bets is UNTRUSTED to the bot (SIGNAL_UNTRUSTED, no bid) — this
    // scene must render the same verdict, not a green "ADDS VALUE" the bot
    // itself does not honour. This is the live shape on this deployment:
    // adds_value_over_market=true, adds_value_on_bet_eligible=false.
    void the_calibrator_node_is_not_promoted_when_it_loses_where_it_bets() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("SIGNAL_UNTRUSTED"))};
        QJsonObject report =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        report.insert(QStringLiteral("adds_value_on_bet_eligible"), false);
        report.insert(QStringLiteral("brier_eligible_full"), 0.2843);
        report.insert(QStringLiteral("brier_eligible_market_mid_raw"), 0.2659);
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("calibrator"));
        QVERIFY(node != nullptr);
        // The full-population Brier is still known/present -- this is not
        // "unscored", it is scored and specifically NOT promoted.
        QVERIFY(node->known);
        QVERIFY(!node->value.contains(QStringLiteral("ADDS VALUE")));
        // Fix round 2, Finding 3: this node prints brier_full 0.0698 vs raw
        // mid 0.0729 -- the model WINNING that population -- so a flat "NO
        // EDGE YET" beside it read as a broken screen. The badge now names
        // the conjunct that actually refused, and the numbers it refused on
        // are on the node.
        QVERIFY(!node->value.contains(QStringLiteral("NO EDGE YET")));
        QVERIFY(node->value.contains(QStringLiteral("NO EDGE WHERE IT BETS")));
        QVERIFY(node->value.contains(QStringLiteral("BET-ELIGIBLE Brier 0.2843 vs mid 0.2659")));
        QCOMPARE(node->role, QStringLiteral("amber"));

        // Same rule, same evidence, for the KXBTC15M scoreboard line and its
        // orbit/KPI node coloring -- not just the threshold calibrator.
        QJsonObject kxbtc15m = kxbtc15m_calibrator_report(
            kNow - 10'000, QJsonObject{{"KXBTC15M-A", directional_prediction(0.7, 0.5)}},
            /*adds_value=*/true, /*scored=*/150);
        kxbtc15m.insert(QStringLiteral("adds_value_on_bet_eligible"), false);
        kxbtc15m.insert(QStringLiteral("brier_eligible_full"), 0.2843);
        kxbtc15m.insert(QStringLiteral("brier_eligible_market_mid_raw"), 0.2659);
        const QString line = kxbtc15m_scoreboard_line(kxbtc15m);
        QVERIFY(!line.contains(QStringLiteral("ADDS VALUE")));
        // Same seam, same fix: the ΔBrier on this line is NEGATIVE (0.2100 vs
        // 0.2400 -- the model ahead of the mid on the full population), so the
        // badge must not claim there is no edge at all, and the evidence the
        // refusal rests on must be readable.
        QVERIFY(!line.contains(QStringLiteral("NO EDGE YET")));
        QVERIFY(line.contains(QStringLiteral("NO EDGE WHERE IT BETS")));
        QVERIFY(line.contains(QStringLiteral("−0.0300")));
        QVERIFY(line.contains(QStringLiteral("BET-ELIGIBLE Brier 0.2843 vs mid 0.2659")));

        // The inspect body a reader opens next carries the same evidence, the
        // prose verdict, and WHICH variant won each board -- with the flag now
        // requiring the same variant on both, a disagreement there is itself a
        // distinct refusal reason.
        QJsonObject split = kxbtc15m;
        split.insert(QStringLiteral("trusted_variant"), QStringLiteral("physics_brti_avg60"));
        split.insert(QStringLiteral("trusted_variant_eligible"), QStringLiteral("physics"));
        const QString detail = outside_info_inspect_detail(split);
        QVERIFY(detail.contains(QStringLiteral("BET-ELIGIBLE Brier 0.2843 vs mid 0.2659")));
        QVERIFY(detail.contains(QStringLiteral(
            "full-population edge only, NOT where it bets — opinion, not signal")));
        QVERIFY(detail.contains(QStringLiteral(
            "trusted_variant (full) physics_brti_avg60 · (bet-eligible) physics")));
        const BotCockpitScene scene_15m =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow, QByteArray(), 0, 0,
                               {}, kxbtc15m);
        const BotCockpitNode* node_15m = scene_15m.node(QStringLiteral("kxbtc15m"));
        QVERIFY(node_15m != nullptr);
        QCOMPARE(node_15m->role, QStringLiteral("amber"));
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
        QVERIFY(scene.census.contains(QStringLiteral("40 threshold")));
        QVERIFY(scene.census.contains(QStringLiteral("0 kxbtc15m")));
        QVERIFY(scene.census.contains(QStringLiteral("0 commodities15m")));
        // Ranked by |edge|: the widest-edge contract is drawn first.
        QVERIFY(scene.columns.first().abs_edge >= scene.columns.last().abs_edge);
    }

    // ── dual-source rain + 15m scoreboard ──────────────────────────────────

    void dual_source_rain_tags_columns_by_family() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000,
                              QJsonObject{{"KXBTCD-26AUG0712-T64000", prediction(0.55, 0.42)}});
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 8'000,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.62, 0.48)}});
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KXBTCD-26AUG0712-T64000"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, dir15);
        QCOMPARE(scene.columns.size(), 2);
        QHash<QString, QString> source_by_ticker;
        for (const auto& column : scene.columns)
            source_by_ticker.insert(column.ticker, column.signal_source);
        QCOMPARE(source_by_ticker.value(QStringLiteral("KXBTCD-26AUG0712-T64000")),
                 QStringLiteral("threshold"));
        QCOMPARE(source_by_ticker.value(QStringLiteral("KXBTC15M-26AUG071230-30")),
                 QStringLiteral("kxbtc15m"));
        QVERIFY(scene.census.contains(QStringLiteral("1 threshold")));
        QVERIFY(scene.census.contains(QStringLiteral("1 kxbtc15m")));
        QVERIFY(scene.census.contains(QStringLiteral("0 commodities15m")));
        QVERIFY(scene.census.contains(QStringLiteral("threshold first")));
        QVERIFY(scene.census.contains(QStringLiteral("ambition KXBTCD")));
        // Paper ambition: strike books lead FLOW even when 15m is present.
        QCOMPARE(scene.columns.at(0).signal_source, QStringLiteral("threshold"));
        QCOMPARE(scene.columns.at(1).signal_source, QStringLiteral("kxbtc15m"));
    }

    void commodities_rain_tags_columns_and_falls_open_bps() {
        QJsonObject commod = kxbtc15m_calibrator_report(
            kNow - 6'000,
            QJsonObject{{"KXGOLD15M-26AUG072115-15", directional_prediction(0.58, 0.45)},
                        {"KXWTI15M-26AUG072115-15", directional_prediction(0.70, 0.48)}});
        commod.insert(QStringLiteral("event"), QStringLiteral("commodities_15m_calibrator"));
        commod.insert(QStringLiteral("settlement_parity"),
                      QJsonObject{{QStringLiteral("checked"), 4},
                                  {QStringLiteral("matched"), 3},
                                  {QStringLiteral("match_rate"), 0.75}});
        QJsonObject commod_hourly = kxbtc15m_calibrator_report(
            kNow - 7'000,
            QJsonObject{{"KXGOLDH-26AUG0712-T2500", prediction(0.61, 0.48)}},
            /*adds_value=*/true, /*scored=*/250, /*brier_full=*/0.12,
            /*brier_mid=*/0.20);
        commod_hourly.insert(QStringLiteral("event"),
                             QStringLiteral("commodities_hourly_calibrator"));
        commod_hourly.insert(QStringLiteral("settlement_parity"),
                             QJsonObject{{QStringLiteral("checked"), 8},
                                         {QStringLiteral("matched"), 8},
                                         {QStringLiteral("match_rate"), 1.0}});
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000,
                              QJsonObject{{"KXBTCD-26AUG0712-T64000", prediction(0.80, 0.40)}});
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 8'000,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.55, 0.50)}});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, dir15, commod, {},
                                {}, commod_hourly);
        QCOMPARE(scene.columns.size(), 5);
        QCOMPARE(scene.columns.at(0).signal_source, QStringLiteral("threshold"));
        QCOMPARE(scene.columns.at(1).signal_source, QStringLiteral("kxbtc15m"));
        QCOMPARE(scene.columns.at(2).signal_source, QStringLiteral("commodities15m"));
        QCOMPARE(scene.columns.at(3).signal_source, QStringLiteral("commodities15m"));
        QCOMPARE(scene.columns.at(4).signal_source, QStringLiteral("commodities_hourly"));
        QCOMPARE(bot_cockpit_source_tag(QStringLiteral("commodities15m")), QStringLiteral("COM"));
        QVERIFY(scene.census.contains(QStringLiteral("2 commodities15m")));
        QVERIFY(scene.census.contains(QStringLiteral("1 commodities_hourly")));
        QVERIFY(scene.census.contains(QStringLiteral("threshold first")));
        // Commodities races use the open glyph (same directional features as BTC 15m).
        QStringList labels;
        for (const auto& glyph : scene.columns.at(2).glyphs) labels << glyph.label;
        QVERIFY(labels.contains(QStringLiteral("open")));
        QVERIFY(!labels.contains(QStringLiteral("sigma")));
        // Metals each get their own orbit box (row 1); cadences fold inside.
        // Gold must never share a tile with silver/oil.
        const BotCockpitNode* gold = scene.node(QStringLiteral("gold"));
        const BotCockpitNode* silver = scene.node(QStringLiteral("silver"));
        const BotCockpitNode* wti = scene.node(QStringLiteral("wti"));
        QVERIFY(gold != nullptr);
        QVERIFY(silver != nullptr);
        QVERIFY(wti != nullptr);
        QCOMPARE(gold->row, 1);
        QCOMPARE(silver->row, 1);
        QCOMPARE(wti->row, 1);
        QVERIFY(gold->label.contains(QStringLiteral("GOLD")));
        QVERIFY(gold->value.contains(QStringLiteral("15m")));
        QVERIFY(gold->value.contains(QStringLiteral("H ")));
        QVERIFY(gold->value.contains(QStringLiteral("D ")));
        QVERIFY(!gold->detail.isEmpty());
        QVERIFY(gold->detail.contains(QStringLiteral("KXGOLD15M")));
        QVERIFY(gold->detail.contains(QStringLiteral("KXGOLDH")));
        // Pooled commodities15m / commodities_hourly tiles are gone.
        QVERIFY(scene.node(QStringLiteral("commodities15m")) == nullptr);
        QVERIFY(scene.node(QStringLiteral("commodities_hourly")) == nullptr);
    }

    void outside_info_inspect_detail_lists_sorted_ablations() {
        const QJsonObject ablations{
            {QStringLiteral("physics_confirm_only"),
             QJsonObject{{QStringLiteral("brier"), 0.11},
                         {QStringLiteral("beats_mid"), true},
                         {QStringLiteral("scored_contracts"), 12}}},
            {QStringLiteral("physics"),
             QJsonObject{{QStringLiteral("brier"), 0.20},
                         {QStringLiteral("beats_mid"), false},
                         {QStringLiteral("scored_contracts"), 20}}},
        };
        const QJsonObject report = kxbtc15m_calibrator_report(
            kNow, {}, /*adds_value=*/false, /*scored=*/20, 0.20, 0.22, {}, ablations);
        const QString detail = outside_info_inspect_detail(report);
        QVERIFY(detail.contains(QStringLiteral("trusted_variant —")));
        QVERIFY(detail.contains(QStringLiteral("ablations (observe")));
        // Sorted keys: physics before physics_confirm_only.
        QVERIFY(detail.indexOf(QStringLiteral("physics  ")) <
                detail.indexOf(QStringLiteral("physics_confirm_only")));
        QVERIFY(detail.contains(QStringLiteral("beats_mid")));
        QCOMPARE(outside_info_inspect_detail({}),
                 QStringLiteral("NO REPORT — this family has not published here yet"));
    }

    void rain_sorts_threshold_before_15m_then_by_edge() {
        // 15m edges are deliberately wider than the narrow threshold book —
        // if sort were |edge|-only, 15m would lead. Threshold (ambition) must
        // still win the family rank, then |edge| within the family.
        const QJsonObject threshold = calibrator_report(
            kNow - 10'000,
            QJsonObject{{"KXBTCD-WIDE", prediction(0.80, 0.40)},   // |edge| 0.40
                        {"KXBTCD-NARROW", prediction(0.50, 0.42)}});  // |edge| 0.08
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 8'000,
            QJsonObject{{"KXBTC15M-26AUG071245-00", directional_prediction(0.55, 0.48)},  // 0.07
                        {"KXBTC15M-26AUG071230-30", directional_prediction(0.70, 0.48)}}); // 0.22
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, dir15);
        QCOMPARE(scene.columns.size(), 4);
        QCOMPARE(scene.columns.at(0).signal_source, QStringLiteral("threshold"));
        QCOMPARE(scene.columns.at(1).signal_source, QStringLiteral("threshold"));
        QCOMPARE(scene.columns.at(2).signal_source, QStringLiteral("kxbtc15m"));
        QCOMPARE(scene.columns.at(3).signal_source, QStringLiteral("kxbtc15m"));
        QCOMPARE(scene.columns.at(0).ticker, QStringLiteral("KXBTCD-WIDE"));
        QCOMPARE(scene.columns.at(1).ticker, QStringLiteral("KXBTCD-NARROW"));
        QCOMPARE(scene.columns.at(2).ticker, QStringLiteral("KXBTC15M-26AUG071230-30"));
        QCOMPARE(scene.columns.at(3).ticker, QStringLiteral("KXBTC15M-26AUG071245-00"));
    }

    void prefer_open_kxbtc15m_keeps_current_when_still_open() {
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.70, 0.48)},
                        {"KXBTC15M-26AUG071245-00", directional_prediction(0.55, 0.48)}});
        QCOMPARE(prefer_open_kxbtc15m_ticker(dir15, QStringLiteral("KXBTC15M-26AUG071245-00")),
                 QStringLiteral("KXBTC15M-26AUG071245-00"));
    }

    void prefer_open_kxbtc15m_picks_widest_edge_when_current_gone() {
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.70, 0.48)},
                        {"KXBTC15M-26AUG071245-00", directional_prediction(0.55, 0.48)}});
        // Current race closed out of the report — follow the widest remaining edge.
        QCOMPARE(prefer_open_kxbtc15m_ticker(dir15, QStringLiteral("KXBTC15M-26AUG071215-15")),
                 QStringLiteral("KXBTC15M-26AUG071230-30"));
        QCOMPARE(prefer_open_kxbtc15m_ticker(dir15, QString()),
                 QStringLiteral("KXBTC15M-26AUG071230-30"));
        QCOMPARE(prefer_open_kxbtc15m_ticker({}, QStringLiteral("KXBTC15M-26AUG071230-30")),
                 QString());
    }

    void threshold_report_kxbtc15m_predictions_are_stripped_from_rain() {
        // Mimic a polluted calibrator.json that still carries a 15m ticker —
        // the cockpit must ignore it the same way kalshi bot does.
        const QJsonObject polluted = calibrator_report(
            kNow - 10'000,
            QJsonObject{{"KXBTCD-26AUG0712-T64000", prediction(0.55, 0.42)},
                        {"KXBTC15M-26AUG071230-30", prediction(0.90, 0.40)}});
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KXBTCD-26AUG0712-T64000"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), polluted, {}, ledger, {}, kNow);
        QCOMPARE(scene.columns.size(), 1);
        QCOMPARE(scene.columns.first().ticker, QStringLiteral("KXBTCD-26AUG0712-T64000"));
        QCOMPARE(scene.columns.first().signal_source, QStringLiteral("threshold"));
    }

    void kxbtc15m_scoreboard_line_shows_scored_delta_and_trust() {
        const QJsonObject ablations{
            {QStringLiteral("physics"),
             QJsonObject{{QStringLiteral("brier"), 0.21},
                         {QStringLiteral("beats_mid"), false},
                         {QStringLiteral("scored_contracts"), 7}}},
            {QStringLiteral("physics_brti_avg60"),
             QJsonObject{{QStringLiteral("brier"), 0.19},
                         {QStringLiteral("beats_mid"), false},
                         {QStringLiteral("scored_contracts"), 4}}},
        };
        const QJsonObject report = kxbtc15m_calibrator_report(
            kNow, {}, /*adds_value=*/false, /*scored=*/7, 0.2100, 0.2400, {}, ablations);
        const QString line = kxbtc15m_scoreboard_line(report);
        QVERIFY(line.contains(QStringLiteral("7/100 scored")));
        QVERIFY(line.contains(QStringLiteral("ΔBrier")));
        QVERIFY(line.contains(QStringLiteral("NO EDGE YET")));
        // Negative Δ (model better than mid) uses a minus sign, not a hyphen
        // that would hide the direction.
        QVERIFY(line.contains(QStringLiteral("−0.0300")));
        QVERIFY(line.contains(QStringLiteral("ablate")));
        QVERIFY(line.contains(QStringLiteral("brti_avg60 n=4")));

        const QJsonObject trusted = kxbtc15m_calibrator_report(
            kNow, {}, /*adds_value=*/true, /*scored=*/120, 0.2000, 0.2400,
            QStringLiteral("physics_confirm_only"));
        const QString trusted_line = kxbtc15m_scoreboard_line(trusted);
        QVERIFY(trusted_line.contains(QStringLiteral("ADDS VALUE")));
        QVERIFY(trusted_line.contains(QStringLiteral("physics_confirm_only")));
    }

    void commodities_scoreboard_line_appends_settlement_parity() {
        QJsonObject report = kxbtc15m_calibrator_report(
            kNow, {}, /*adds_value=*/false, /*scored=*/3, 0.30, 0.10);
        report.insert(QStringLiteral("event"), QStringLiteral("commodities_15m_calibrator"));
        report.insert(QStringLiteral("settlement_parity"),
                      QJsonObject{{QStringLiteral("checked"), 10},
                                  {QStringLiteral("matched"), 9},
                                  {QStringLiteral("match_rate"), 0.9}});
        const QString line = commodities_15m_scoreboard_line(report);
        QVERIFY(line.contains(QStringLiteral("parity Pyth↔Kalshi 9/10 (90%)")));
    }

    void cockpit_orbit_and_kpi_surface_the_15m_scoreboard() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const QJsonObject dir15 = kxbtc15m_calibrator_report(kNow - 8'000, {}, false, 7);
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live(), dir15);
        const BotCockpitNode* node = scene.node(QStringLiteral("kxbtc15m"));
        QVERIFY(node != nullptr);
        QVERIFY(node->value.contains(QStringLiteral("7/100 scored")));
        QCOMPARE(node->role, QStringLiteral("amber"));
        const QString strip = scene.kpi.join(QStringLiteral(" | "));
        QVERIFY(strip.contains(QStringLiteral("BTC 15M|D")));
        QVERIFY(strip.contains(QStringLiteral("7/100 scored")));
    }

    void stale_threshold_freezes_only_threshold_columns() {
        const QJsonObject stale_threshold =
            calibrator_report(kNow - 200'000,
                              QJsonObject{{"KXBTCD-26AUG0712-T64000", prediction(0.55, 0.42)}});
        const QJsonObject fresh_15m = kxbtc15m_calibrator_report(
            kNow - 5'000,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.62, 0.48)}});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), stale_threshold, {}, ledger, {}, kNow,
                                QByteArray(), kBotCockpitMaxColumns, kBotCockpitMaxPulses,
                                feed_live(), fresh_15m);
        QHash<QString, bool> frozen_by_ticker;
        for (const auto& column : scene.columns)
            frozen_by_ticker.insert(column.ticker, column.frozen);
        QVERIFY(frozen_by_ticker.value(QStringLiteral("KXBTCD-26AUG0712-T64000")));
        QVERIFY(!frozen_by_ticker.value(QStringLiteral("KXBTC15M-26AUG071230-30")));
        // Rain still moves because the 15m column is live.
        QVERIFY(scene.motion);
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_threshold")), QStringLiteral("red"));
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_15m")), QStringLiteral("amber"));
        QVERIFY(scene.kxbtc15m_hero_line.contains(QStringLiteral("7/100 scored")));
    }

    void directional_columns_fall_open_bps_not_sigma() {
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 5'000,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.62, 0.48, -8.5)}});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, dir15);
        QCOMPARE(scene.columns.size(), 1);
        QCOMPARE(scene.columns.first().signal_source, QStringLiteral("kxbtc15m"));
        QCOMPARE(bot_cockpit_source_tag(scene.columns.first().signal_source),
                 QStringLiteral("15m"));
        QStringList labels;
        for (const auto& glyph : scene.columns.first().glyphs) labels << glyph.label;
        QVERIFY(labels.contains(QStringLiteral("open")));
        QVERIFY(!labels.contains(QStringLiteral("sigma")));
        for (const auto& glyph : scene.columns.first().glyphs) {
            if (glyph.label != QStringLiteral("open")) continue;
            QVERIFY(glyph.known);
            QCOMPARE(glyph.value, -8.5);
            QCOMPARE(glyph.text, QStringLiteral("-8.5"));
        }
    }

    void threshold_columns_still_fall_sigma() {
        const QJsonObject report =
            calibrator_report(kNow - 5'000, QJsonObject{{"KX-A", prediction(0.55, 0.42, 3.5)}});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        QStringList labels;
        for (const auto& glyph : scene.columns.first().glyphs) labels << glyph.label;
        QVERIFY(labels.contains(QStringLiteral("sigma")));
        QVERIFY(!labels.contains(QStringLiteral("open")));
        QCOMPARE(bot_cockpit_source_tag(scene.columns.first().signal_source),
                 QStringLiteral("THR"));
    }

    void split_calibrate_chips_expose_each_family() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}},
                              /*adds_value=*/true);
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 8'000,
            QJsonObject{{"KXBTC15M-26AUG071230-30", directional_prediction(0.62, 0.48)}},
            /*adds_value=*/false, /*scored=*/40);
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live(), dir15);
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_threshold")), QStringLiteral("green"));
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_15m")), QStringLiteral("amber"));
        for (const auto& st : scene.health_stages) {
            if (st.id == QStringLiteral("calibrate_15m"))
                QCOMPARE(st.value, QStringLiteral("40/100"));
        }
        QCOMPARE(scene.health_stages.size(), 9); // HARVEST THR 15M COM COM-H COM-D BTC-D DECIDE SETTLE
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_commodities_daily")),
                 QStringLiteral("grey"));
        QCOMPARE(stage_value(scene, QStringLiteral("calibrate_commodities_daily")),
                 QStringLiteral("idle"));
        // Idle family when only threshold rain is present.
        const BotCockpitScene thr_only =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live());
        QCOMPARE(stage_role(thr_only, QStringLiteral("calibrate_15m")), QStringLiteral("grey"));
        QCOMPARE(stage_value(thr_only, QStringLiteral("calibrate_15m")), QStringLiteral("idle"));
    }

    void commodities_daily_calibrate_chip_lights_when_rain_has_com_d() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        QJsonObject com_d =
            calibrator_report(kNow - 8'000,
                              QJsonObject{{"KXGOLDD-26AUG1017-T4497", prediction(0.55, 0.42)}},
                              /*adds_value=*/false);
        com_d.insert(QStringLiteral("event"), QStringLiteral("commodities_daily_calibrator"));
        com_d.insert(QStringLiteral("scored_contracts"), 12);
        com_d.insert(QStringLiteral("min_scored_contracts"), 100);
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene = present_bot_cockpit(
            panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, feed_live(), {}, {}, {}, {}, {}, com_d);
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_commodities_daily")),
                 QStringLiteral("amber"));
        QCOMPARE(stage_value(scene, QStringLiteral("calibrate_commodities_daily")),
                 QStringLiteral("12/100"));
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_commodities15m")),
                 QStringLiteral("grey"));
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_commodities_hourly")),
                 QStringLiteral("grey"));
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_kxbtc_daily")), QStringLiteral("grey"));
    }

    // Empty-but-present hourly report must not hide as idle — watch the file.
    void empty_present_hourly_report_lights_com_h_chip_not_idle() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        QJsonObject com_h = calibrator_report(kNow - 8'000, {}, /*adds_value=*/false);
        com_h.insert(QStringLiteral("event"), QStringLiteral("commodities_hourly_calibrator"));
        com_h.insert(QStringLiteral("scored_contracts"), 0);
        com_h.insert(QStringLiteral("min_scored_contracts"), 100);
        com_h.insert(QStringLiteral("predictions"), QJsonObject{});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene = present_bot_cockpit(
            panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, feed_live(), {}, {}, {}, {}, com_h);
        QCOMPARE(stage_role(scene, QStringLiteral("calibrate_commodities_hourly")),
                 QStringLiteral("amber"));
        QCOMPARE(stage_value(scene, QStringLiteral("calibrate_commodities_hourly")),
                 QStringLiteral("0/100"));
        // Stale empty report must redden the chip (not stay idle).
        QJsonObject stale_h = com_h;
        stale_h.insert(QStringLiteral("generated_at_ms"),
                       double(kNow - bot_cockpit_freeze_bound_ms()));
        const BotCockpitScene stale = present_bot_cockpit(
            panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, feed_live(), {}, {}, {}, {}, stale_h);
        QCOMPARE(stage_role(stale, QStringLiteral("calibrate_commodities_hourly")),
                 QStringLiteral("red"));
        QCOMPARE(stage_value(stale, QStringLiteral("calibrate_commodities_hourly")),
                 QStringLiteral("stale"));
    }

    void com_kpi_role_is_worst_of_15m_hourly_and_daily() {
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}},
                              /*adds_value=*/true);
        QJsonObject com15 = calibrator_report(kNow - 8'000, {}, /*adds_value=*/true);
        com15.insert(QStringLiteral("event"), QStringLiteral("commodities_15m_calibrator"));
        com15.insert(QStringLiteral("brier_full"), 0.20);
        com15.insert(QStringLiteral("brier_market_mid_raw"), 0.22);
        QJsonObject com_d = calibrator_report(kNow - 8'000, {}, /*adds_value=*/false);
        com_d.insert(QStringLiteral("event"), QStringLiteral("commodities_daily_calibrator"));
        com_d.insert(QStringLiteral("brier_full"), 0.25);
        com_d.insert(QStringLiteral("brier_market_mid_raw"), 0.22);
        const BotCockpitScene scene = present_bot_cockpit(
            panel_for(passing_ledger()), threshold, {}, passing_ledger(), {}, kNow, QByteArray(),
            kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live(), {}, com15, {}, {}, {}, com_d);
        QVERIFY(!scene.kpi.isEmpty());
        int com_idx = -1;
        for (int i = 0; i < scene.kpi.size(); ++i) {
            if (scene.kpi.at(i).startsWith(QStringLiteral("COM 15m|H|D"))) {
                com_idx = i;
                break;
            }
        }
        QVERIFY(com_idx >= 0);
        QCOMPARE(scene.kpi_roles.at(com_idx), QStringLiteral("amber"));
    }

    void threshold_hero_pins_ambition_scoreboard_above_rain() {
        const QJsonObject threshold =
            calibrator_report(kNow - 5'000, {}, /*adds_value=*/true);
        const QJsonObject dir15 =
            kxbtc15m_calibrator_report(kNow - 5'000, {}, /*adds_value=*/false, 7);
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(passing_ledger()), threshold, {}, passing_ledger(), {},
                                kNow, QByteArray(), kBotCockpitMaxColumns, kBotCockpitMaxPulses,
                                feed_live(), dir15);
        QVERIFY(scene.threshold_hero_line.startsWith(QStringLiteral("KXBTCD ·")));
        QVERIFY(scene.threshold_hero_line.contains(QStringLiteral("318/")));
        QCOMPARE(scene.threshold_hero_scored, 318);
        QCOMPARE(scene.threshold_hero_role, QStringLiteral("green"));
        QVERIFY(scene.threshold_hero_known);
        // 15m remains on the strip / fallback fields, not the ambition pin.
        QVERIFY(scene.kxbtc15m_hero_line.startsWith(QStringLiteral("KXBTC15M ·")));
        QVERIFY(scene.kxbtc15m_hero_line.contains(QStringLiteral("7/100 scored")));
    }

    void kxbtc15m_hero_fills_when_threshold_report_missing() {
        const QJsonObject dir15 =
            kxbtc15m_calibrator_report(kNow - 5'000, {}, /*adds_value=*/false, 7);
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(passing_ledger()), {}, {}, passing_ledger(), {}, kNow,
                                QByteArray(), kBotCockpitMaxColumns, kBotCockpitMaxPulses,
                                feed_live(), dir15);
        QVERIFY(scene.threshold_hero_line.isEmpty());
        QVERIFY(scene.kxbtc15m_hero_line.startsWith(QStringLiteral("KXBTC15M ·")));
        QVERIFY(scene.kxbtc15m_hero_line.contains(QStringLiteral("7/100 scored")));
        QCOMPARE(scene.kxbtc15m_hero_scored, 7);
        QCOMPARE(scene.kxbtc15m_hero_floor, 100);
        QCOMPARE(scene.kxbtc15m_hero_role, QStringLiteral("amber"));
        QVERIFY(scene.kxbtc15m_hero_known);
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

    // ── health-first classifier (Task 1) ────────────────────────────────────

    // 1. Feed down turns the banner RED even though the loop is ticking paper.
    void feed_down_reddens_the_banner_over_a_ticking_loop() {
        const QJsonArray ledger = bidding_ledger();
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), /*adds_value=*/true);
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed_down());
        QCOMPARE(s.health_role, QStringLiteral("red"));
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("red"));
        QVERIFY(s.health_banner.contains(QStringLiteral("FEED")));
        // Neuter: the same inputs with a LIVE feed are not red.
        const BotCockpitScene ok = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                       QByteArray(), kBotCockpitMaxColumns,
                                                       kBotCockpitMaxPulses, feed_live());
        QVERIFY(ok.health_role != QStringLiteral("red"));
    }

    // 2. Fresh feed + fresh-but-untrusted report => AMBER "no edge", not red.
    void untrusted_signal_is_amber_not_red() {
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), /*adds_value=*/false);
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed_live());
        QCOMPARE(s.health_role, QStringLiteral("amber"));
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("green"));
        QCOMPARE(stage_role(s, QStringLiteral("calibrate_threshold")), QStringLiteral("amber"));
        QCOMPARE(stage_role(s, QStringLiteral("calibrate_15m")), QStringLiteral("grey"));
        QCOMPARE(stage_value(s, QStringLiteral("calibrate_15m")), QStringLiteral("idle"));
    }

    // 3. Report older than 120s => THR red, and a REPORT_STALE loop does
    // NOT redden DECIDE.
    void stale_report_reddens_calibrate_not_decide() {
        const QJsonObject report = calibrator_report(kNow - 200'000, one_trusted_prediction(), true);
        const QJsonArray ledger = report_stale_ledger();
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed_live());
        QCOMPARE(stage_role(s, QStringLiteral("calibrate_threshold")), QStringLiteral("red"));
        QVERIFY(stage_role(s, QStringLiteral("decide")) != QStringLiteral("red"));
    }

    // 4. Fed + trusted + a recent bid => GREEN acting.
    void acting_is_green() {
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
        const QJsonArray ledger = bidding_ledger();
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed_live());
        QCOMPARE(s.health_role, QStringLiteral("green"));
        for (const auto& id : {"harvest", "calibrate_threshold", "decide"})
            QCOMPARE(stage_role(s, QString::fromLatin1(id)), QStringLiteral("green"));
        QCOMPARE(stage_role(s, QStringLiteral("calibrate_15m")), QStringLiteral("grey"));
        QCOMPARE(stage_value(s, QStringLiteral("calibrate_15m")), QStringLiteral("idle"));
    }

    // 4b. A bid sitting in the byte-window ledger tail but OLDER than
    // kDecideBidRecencyMs must NOT read DECIDE green. The loop is still
    // running (a recent PASS keeps it "running"), so this is amber
    // "passing", never red and never a stale green — an old bid on a running
    // loop just means nothing is being bid on right now.
    void an_old_bid_in_the_tail_reads_decide_amber_not_green() {
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
        const QJsonArray ledger = old_bid_ledger();
        const KalshiBotPanelView panel = panel_for(ledger);
        QCOMPARE(panel.state, QStringLiteral("running"));
        const BotCockpitScene s = present_bot_cockpit(panel, report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed_live());
        QCOMPARE(stage_role(s, QStringLiteral("decide")), QStringLiteral("amber"));
        QVERIFY(s.health_role != QStringLiteral("green"));
        QVERIFY(s.health_role != QStringLiteral("red"));
    }

    // 5. Kill switch => DECIDE red "LOOP STOPPED", regardless of feed.
    void stopped_loop_reddens_decide() {
        openmarketterminal::services::prediction::kalshi_ns::KalshiBotStopFile stop;
        stop.engaged = true;
        stop.ts_ms = kNow - 5'000;
        const QJsonArray ledger = bidding_ledger();
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger, {}, {}, kNow, stop),
                                                      calibrator_report(kNow, one_trusted_prediction(), true),
                                                      {}, ledger, {}, kNow, QByteArray(),
                                                      kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live());
        QCOMPARE(stage_role(s, QStringLiteral("decide")), QStringLiteral("red"));
        QVERIFY(s.health_banner.contains(QStringLiteral("STOPPED")));
    }

    // 6. Unknown feed (view supplied nothing) => HARVEST grey/unknown, never a
    // false green.
    void unknown_feed_is_not_green() {
        const BotCockpitScene s = present_bot_cockpit(panel_for(bidding_ledger()),
                                                      calibrator_report(kNow, one_trusted_prediction(), true),
                                                      {}, bidding_ledger(), {}, kNow);  // default feed {}
        QVERIFY(stage_role(s, QStringLiteral("harvest")) != QStringLiteral("green"));
        // Not just "not green" — an unreadable feed reads grey/unknown, not
        // red either: the view said nothing, so nothing is claimed.
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("grey"));
    }

    // ── FIX 4: readable rain column labels (additive) ──────────────────────

    // KXBTC15M contracts show their own close time HH:MM, parsed out of the
    // ticker's YYMMMDDHHMM segment — not the raw `26JUL311730-30` tail.
    void column_head_shows_the_15m_contracts_close_time() {
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXBTC15M-26JUL311730-30")),
                 QStringLiteral("17:30"));
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXGOLD15M-26AUG072115-15")),
                 QStringLiteral("21:15"));
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXWTI15M-26AUG072030-30")),
                 QStringLiteral("20:30"));
    }

    void fifteen_m_close_ms_parses_eastern_close() {
        // kNow is 2026-07-25 13:20 ET — 12:00 ET that day is closed; Aug 7 stays open.
        const qint64 closed =
            bot_cockpit_15m_close_ms(QStringLiteral("KXBTC15M-26JUL251200-00"));
        const qint64 open =
            bot_cockpit_15m_close_ms(QStringLiteral("KXBTC15M-26AUG071230-30"));
        QVERIFY(closed > 0);
        QVERIFY(open > 0);
        QVERIFY(!bot_cockpit_15m_still_open(QStringLiteral("KXBTC15M-26JUL251200-00"), kNow));
        QVERIFY(bot_cockpit_15m_still_open(QStringLiteral("KXBTC15M-26AUG071230-30"), kNow));
        // Threshold / unparseable tickers are not filtered as closed 15m.
        QVERIFY(bot_cockpit_15m_still_open(QStringLiteral("KXBTCD-26AUG0712-T64000"), kNow));
    }

    void rain_omits_closed_15m_contracts_from_flow() {
        // Stale calibrator may still list expired races; FLOW must not paint them.
        const QJsonObject dir15 = kxbtc15m_calibrator_report(
            kNow - 8'000,
            QJsonObject{{"KXBTC15M-26JUL251200-00", directional_prediction(0.70, 0.48)},
                        {"KXBTC15M-26AUG071230-30", directional_prediction(0.62, 0.48)}});
        const QJsonObject threshold =
            calibrator_report(kNow - 10'000,
                              QJsonObject{{"KXBTCD-26AUG0712-T64000", prediction(0.55, 0.42)}});
        const QJsonArray ledger = passing_ledger();
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), threshold, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, dir15);
        QCOMPARE(scene.columns.size(), 2);
        QStringList tickers;
        for (const auto& column : scene.columns) tickers << column.ticker;
        QVERIFY(!tickers.contains(QStringLiteral("KXBTC15M-26JUL251200-00")));
        QVERIFY(tickers.contains(QStringLiteral("KXBTC15M-26AUG071230-30")));
        QVERIFY(tickers.contains(QStringLiteral("KXBTCD-26AUG0712-T64000")));
        QVERIFY(scene.census.contains(QStringLiteral("1 closed 15m omitted")));
        QVERIFY(scene.census.contains(QStringLiteral("1 kxbtc15m")));
    }

    // KXBTCD threshold contracts show their strike compactly.
    void column_head_shows_the_threshold_contracts_strike_compactly() {
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXBTCD-26JUL3118-T62899.99")),
                 QStringLiteral("$62.9k"));
    }

    // Multi-cadence commodity / BTC floor books: underlier chip + strike.
    void column_head_shows_commodity_and_btc_daily_strikes() {
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXGOLDD-26AUG1017-T4497")),
                 QStringLiteral("AU $4.5k"));
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXSILVERH-26AUG0915-T64.25")),
                 QStringLiteral("AG $64.25"));
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXWTI-26AUG1017-T72.5")),
                 QStringLiteral("CL $72.50"));
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXBTC-26AUG0917-T65000")),
                 QStringLiteral("$65.0k"));
    }

    // An unrecognized ticker family falls back to the previous behavior
    // rather than mislabeling itself.
    void column_head_falls_back_for_an_unrecognized_family() {
        QCOMPARE(bot_cockpit_column_head(QStringLiteral("KXSOMETHINGELSE-26JUL31-ABCD")),
                 QStringLiteral("ABCD"));
    }

    // ── FIX 3: de-bounce HARVEST on the market-event age, not a single
    // connected blip ────────────────────────────────────────────────────────

    // (a) A momentary websocket disconnect beside a still-FRESH market event
    // (age 5s) must not flash HARVEST red — the real down signal is the
    // event age going stale, not a single dropped connected bit. This is
    // amber ("brief disconnect, events still fresh"), not red.
    void a_momentary_disconnect_with_fresh_events_is_amber_not_red() {
        const BotCockpitFeedHealth feed{/*readable=*/true, /*ws_connected=*/false,
                                        /*credentials_ok=*/true, /*newest_event_age_ms=*/5'000, {}};
        const QJsonArray ledger = bidding_ledger();
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed);
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("amber"));
    }

    // (b) A disconnect beside a genuinely STALE event (age 6min) is still a
    // real outage — red, not de-bounced away.
    void a_disconnect_with_stale_events_is_still_red() {
        const BotCockpitFeedHealth feed{/*readable=*/true, /*ws_connected=*/false,
                                        /*credentials_ok=*/true, /*newest_event_age_ms=*/360'000, {}};
        const QJsonArray ledger = bidding_ledger();
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed);
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("red"));
    }

    // ── FIX 2: grey HARVEST never greens the overall banner ────────────────

    // A grey (unknown) HARVEST is neither red nor amber, so the ladder used
    // to fall through past it straight to GREEN "ACTING" whenever CALIBRATE
    // and DECIDE were both healthy — an unreadable feed would then be
    // rendered as a fully healthy pipeline. Grey HARVEST must cap the
    // overall banner at amber (or grey), never green.
    void grey_harvest_caps_the_overall_banner_never_green() {
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), /*adds_value=*/true);
        const QJsonArray ledger = bidding_ledger();
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {},
                                                      kNow);  // default feed {} => harvest grey
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("grey"));
        QCOMPARE(stage_role(s, QStringLiteral("calibrate_threshold")), QStringLiteral("green"));
        QCOMPARE(stage_role(s, QStringLiteral("decide")), QStringLiteral("green"));
        QVERIFY(s.health_role != QStringLiteral("green"));
        // Neuter: the same inputs with a live feed DO read overall green.
        const BotCockpitScene ok = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                       QByteArray(), kBotCockpitMaxColumns,
                                                       kBotCockpitMaxPulses, feed_live());
        QCOMPARE(ok.health_role, QStringLiteral("green"));
    }

    // ── FIX 1: HARVEST keys off a MARKET-data timestamp, not the
    // CF-Benchmarks-conflated `last_event_at` ──────────────────────────────

    // `last_event_at` is bumped by BOTH real market events AND the
    // CF-Benchmarks BRTI tick, so a feed with a fresh `last_event_at` but a
    // stale `last_market_event_at` is exactly "connected, market silent" —
    // BRTI keeps ticking while the tradeable feed has gone quiet. The reader
    // must age off the market-only stamp: reading `last_event_at` here would
    // (wrongly) report a 2s-old feed and green HARVEST.
    void feed_health_reader_ages_off_market_event_not_conflated_last_event_at() {
        const QJsonObject engine{
            {"connected", true},
            {"credentials", true},
            {"last_event_at", QDateTime::fromMSecsSinceEpoch(kNow - 2'000, Qt::UTC)
                                  .toString(Qt::ISODateWithMs)},
            {"last_market_event_at", QDateTime::fromMSecsSinceEpoch(kNow - 360'000, Qt::UTC)
                                          .toString(Qt::ISODateWithMs)}};
        const BotCockpitFeedHealth feed =
            parse_bot_cockpit_feed_health(engine, /*readable=*/true, kNow, /*fallback_age_ms=*/-1);
        QVERIFY(feed.newest_event_age_ms >= 300'000);

        // Fed into the classifier: connected but market-stale still reds
        // HARVEST — a connected-but-market-silent feed cannot green.
        const QJsonArray ledger = bidding_ledger();
        const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
        const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                      QByteArray(), kBotCockpitMaxColumns,
                                                      kBotCockpitMaxPulses, feed);
        QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("red"));
        QVERIFY(s.health_role != QStringLiteral("green"));
    }

    // When the engine has no `last_market_event_at` at all (an older engine
    // build, or one that has connected but not yet ticked), the reader falls
    // back to the caller-supplied age (e.g. the kalshi-tickers.jsonl tail)
    // rather than silently reading `last_event_at`.
    void feed_health_reader_falls_back_when_market_event_absent() {
        const QJsonObject engine{{"connected", true}, {"credentials", true}};
        const BotCockpitFeedHealth feed =
            parse_bot_cockpit_feed_health(engine, /*readable=*/true, kNow, /*fallback_age_ms=*/4'000);
        QCOMPARE(feed.newest_event_age_ms, qint64(4'000));
    }

    // ── the redesign is additive: pre-existing fields are untouched ────────

    void health_fields_are_additive_and_leave_the_rest_of_the_scene_unchanged() {
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KX-A"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const QJsonObject report = calibrator_report(
            kNow - 10'000, QJsonObject{{"KX-A", prediction(0.55, 0.42)}});
        const BotCockpitScene baseline =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow);
        const BotCockpitScene healthy =
            present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow, QByteArray(),
                                kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live());
        // Supplying a feed cannot move any pre-existing field.
        QCOMPARE(healthy.mood, baseline.mood);
        QCOMPARE(healthy.banner, baseline.banner);
        QCOMPARE(healthy.mood_reason, baseline.mood_reason);
        QCOMPARE(healthy.columns.size(), baseline.columns.size());
        QCOMPARE(healthy.columns.first().ticker, baseline.columns.first().ticker);
        QCOMPARE(healthy.columns.first().frozen, baseline.columns.first().frozen);
        QCOMPARE(healthy.columns_total, baseline.columns_total);
        QCOMPARE(healthy.census, baseline.census);
        QCOMPARE(healthy.nodes.size(), baseline.nodes.size());
        QCOMPARE(healthy.pulses.size(), baseline.pulses.size());
        QCOMPARE(healthy.lessons, baseline.lessons);
        QCOMPARE(healthy.envelope, baseline.envelope);
        QCOMPARE(healthy.kpi, baseline.kpi);
        // Pin the concrete values for this fixture too, so a future change to
        // a pre-existing field cannot slip through even if the health fields
        // happened to move in lockstep with it (a `baseline`-vs-`healthy`
        // comparison alone cannot catch the classifier corrupting a field the
        // SAME way in both calls).
        QCOMPARE(baseline.mood, QString::fromLatin1(kBotCockpitMoodPaper));
        QVERIFY(baseline.banner.startsWith(QStringLiteral("PAPER")));
        QCOMPARE(baseline.columns.size(), 1);
        QVERIFY(!baseline.columns.first().frozen);
        QCOMPARE(baseline.columns_total, 1);
        QCOMPARE(baseline.census,
                 QStringLiteral("1 watched contracts · all drawn · L→R · threshold first · ambition "
                                "KXBTCD · 1 threshold · 0 kxbtc15m · 0 commodities15m · 0 "
                                "commodities_hourly · 0 commodities_daily · 0 kxbtc_daily"));
        // Row 0 additionally includes the structural corridor watch + its own
        // paper-only gate. They are always visible, even before evidence.
        // Row 1: GOLD, SILVER, WTI (3). Total 10 — metals never share a box.
        QCOMPARE(baseline.nodes.size(), 13);
        QVERIFY(!baseline.node(QStringLiteral("kxbtc15m"))->detail.isEmpty());
        QVERIFY(!baseline.node(QStringLiteral("gold"))->detail.isEmpty());
        QVERIFY(baseline.node(QStringLiteral("calibrator")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("kxbtc15m")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("gold")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("silver")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("wti")) != nullptr);
        QCOMPARE(baseline.node(QStringLiteral("calibrator"))->row, 0);
        QCOMPARE(baseline.node(QStringLiteral("gold"))->row, 1);
        QVERIFY(baseline.node(QStringLiteral("commodities15m")) == nullptr);
        QVERIFY(baseline.node(QStringLiteral("commodities_hourly")) == nullptr);
        QVERIFY(baseline.node(QStringLiteral("brier_split")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("settlements")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("gate")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("kill_switch")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("exposure")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("btc_corridor")) != nullptr);
        QVERIFY(baseline.node(QStringLiteral("corridor_gate")) != nullptr);
        QCOMPARE(baseline.pulses.size(), 1);
        QCOMPARE(baseline.pulses.first().kind, QStringLiteral("calibrator"));
        QVERIFY(!baseline.lessons_available);
        QCOMPARE(baseline.lessons.size(), 1);
        QVERIFY(baseline.lessons.first().contains(QStringLiteral("UNAVAILABLE")));
        QVERIFY(!baseline.kpi_available);
        // Decision-rate + BTC 15M|D + COM 15m|H|D + model|bot Brier + postmortem.
        QCOMPARE(baseline.kpi.size(), 6);
        QVERIFY(baseline.kpi.at(baseline.kpi.size() - 4).startsWith(QStringLiteral("BTC 15M|D")));
        QVERIFY(baseline.kpi.at(baseline.kpi.size() - 3).startsWith(QStringLiteral("COM 15m|H|D")));
        QVERIFY(baseline.kpi.at(baseline.kpi.size() - 2).startsWith(QStringLiteral("BRIER model")));
        QVERIFY(baseline.kpi.last().startsWith(QStringLiteral("PM ")));
        // PM KPI inspect body is always present (even UNAVAILABLE) — no 10th node.
        QVERIFY(!baseline.postmortem_detail.isEmpty());
        QVERIFY(baseline.postmortem_detail.contains(QStringLiteral("Inspect only")) ||
                baseline.postmortem_detail.contains(QStringLiteral("inspect only")));
    }

    void postmortem_inspect_detail_lists_modes_worst_losses_and_lessons() {
        const QJsonObject summary{
            {QStringLiteral("settled"), 10},
            {QStringLiteral("wins"), 4},
            {QStringLiteral("losses"), 5},
            {QStringLiteral("early_exits"), 1},
            {QStringLiteral("net_realized_pnl_usd"), -12.5},
            {QStringLiteral("fees_usd"), 1.2},
            {QStringLiteral("mean_win_usd"), 0.9},
            {QStringLiteral("mean_loss_usd"), -1.8},
            {QStringLiteral("loss_primary_modes"),
             QJsonArray{QJsonArray{QStringLiteral("cheap_no_crushed_by_yes"), 3},
                        QJsonArray{QStringLiteral("favourite_lost_full_stake"), 2}}},
            {QStringLiteral("worst_losses"),
             QJsonArray{QJsonObject{{QStringLiteral("pnl"), -2.1},
                                    {QStringLiteral("ticker"), QStringLiteral("KXBTC15M-X")},
                                    {QStringLiteral("side"), QStringLiteral("NO")},
                                    {QStringLiteral("price"), 0.1},
                                    {QStringLiteral("result"), QStringLiteral("YES")},
                                    {QStringLiteral("mode"), QStringLiteral("cheap_no_crushed_by_yes")},
                                    {QStringLiteral("gamma"), QStringLiteral("mid_stable_wrong_thesis")},
                                    {QStringLiteral("quote"), QStringLiteral("cross")},
                                    {QStringLiteral("lessons"),
                                     QJsonArray{QStringLiteral(
                                         "YES mid stayed near entry — thesis was wrong")}}}}},
            {QStringLiteral("recommendations"),
             QJsonArray{QJsonObject{{QStringLiteral("priority"), 3},
                                    {QStringLiteral("id"), QStringLiteral("fade_yes_near_close")},
                                    {QStringLiteral("claim"), QStringLiteral("NO fades of high YES")},
                                    {QStringLiteral("action"),
                                     QStringLiteral("Ban unless lead/BRTI confirms")}}}},
            {QStringLiteral("measurement"),
             QJsonObject{{QStringLiteral("fade_ban_lifts"), 2},
                         {QStringLiteral("early_exits"), 1},
                         {QStringLiteral("cheap_no_crushed_by_yes"), 3},
                         {QStringLiteral("favourite_lost_full_stake"), 2},
                         {QStringLiteral("wrong_side_after_crossing_spread"), 1},
                         {QStringLiteral("settlements_with_mid_path"), 4}}},
        };
        const QString detail = postmortem_inspect_detail(summary);
        QVERIFY(detail.contains(QStringLiteral("BID POSTMORTEM")));
        QVERIFY(detail.contains(QStringLiteral("cheap_no_crushed_by_yes")));
        QVERIFY(detail.contains(QStringLiteral("KXBTC15M-X")));
        QVERIFY(detail.contains(QStringLiteral("thesis was wrong")));
        QVERIFY(detail.contains(QStringLiteral("fade_yes_near_close")));
        QVERIFY(detail.contains(QStringLiteral("fade_lifts=2")));
        QVERIFY(detail.contains(QStringLiteral("never arms")));

        QJsonObject current = summary;
        current.insert(QStringLiteral("cohort"), QStringLiteral("current_rules"));
        current.insert(QStringLiteral("settled"), 2);
        current.insert(QStringLiteral("wins"), 0);
        current.insert(QStringLiteral("losses"), 0);
        current.insert(QStringLiteral("early_exits"), 2);
        current.insert(QStringLiteral("net_realized_pnl_usd"), 0.5);
        QJsonObject historic = summary;
        historic.insert(QStringLiteral("cohort"), QStringLiteral("historic"));
        const QString dual = postmortem_inspect_detail(current, historic);
        QVERIFY(dual.contains(QStringLiteral("CURRENT RULES")));
        QVERIFY(dual.contains(QStringLiteral("HISTORIC")));
        QCOMPARE(postmortem_kpi_line(current),
                 QStringLiteral("PM CUR 2 cashouts · net $0.50"));
        QVERIFY(postmortem_kpi_line(historic).startsWith(QStringLiteral("PM HIST ")));
    }

    void settlements_node_prefers_current_rules_over_lifetime_gate() {
        const QJsonArray ledger = passing_ledger();
        QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 90, 46, 44, -35.4, 48.33);
        QJsonObject current{
            {QStringLiteral("cohort"), QStringLiteral("current_rules")},
            {QStringLiteral("settled"), 3},
            {QStringLiteral("wins"), 0},
            {QStringLiteral("losses"), 0},
            {QStringLiteral("early_exits"), 3},
            {QStringLiteral("net_realized_pnl_usd"), 0.51},
            {QStringLiteral("fees_usd"), 0.15},
        };
        QJsonObject historic{
            {QStringLiteral("cohort"), QStringLiteral("historic")},
            {QStringLiteral("settled"), 98},
            {QStringLiteral("wins"), 46},
            {QStringLiteral("losses"), 44},
            {QStringLiteral("net_realized_pnl_usd"), -36.4},
            {QStringLiteral("fees_usd"), 5.38},
        };
        const BotCockpitScene scene = present_bot_cockpit(
            panel_for(ledger, gate), {}, gate, ledger, {}, kNow, QByteArray(),
            kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, {}, {}, current, historic);
        const BotCockpitNode* settlements = scene.node(QStringLiteral("settlements"));
        QVERIFY(settlements != nullptr);
        QVERIFY(settlements->label.contains(QStringLiteral("CURRENT RULES")));
        QVERIFY(settlements->value.contains(QStringLiteral("CURRENT RULES")));
        QVERIFY(settlements->value.contains(QStringLiteral("0.51")) ||
                settlements->value.contains(QStringLiteral("$0.51")));
        QVERIFY(!settlements->value.startsWith(QStringLiteral("90 settled")));
        const BotCockpitNode* gate_node = scene.node(QStringLiteral("gate"));
        QVERIFY(gate_node != nullptr);
        QVERIFY(gate_node->label.contains(QStringLiteral("LIFETIME")));
        bool saw_cur_net = false;
        for (const QString& line : scene.kpi) {
            if (line.startsWith(QStringLiteral("NET CUR"))) saw_cur_net = true;
        }
        QVERIFY(saw_cur_net);
    }

    void brier_split_node_contrasts_model_and_bot_settled() {
        const QJsonArray ledger = passing_ledger();
        const QJsonObject report =
            calibrator_report(kNow, QJsonObject{{"KX-A", prediction(0.55, 0.42)}}, true);
        QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 90, 46, 44, -37.8, 48.33);
        // Force the bot-settled Brier worse than mid (matches live FAIL).
        QJsonArray criteria = gate.value(QStringLiteral("criteria")).toArray();
        for (int i = 0; i < criteria.size(); ++i) {
            QJsonObject c = criteria.at(i).toObject();
            if (c.value(QStringLiteral("id")).toString() != QLatin1String("brier_beats_market"))
                continue;
            c.insert(QStringLiteral("brier_bot"), 0.2617);
            c.insert(QStringLiteral("brier_market_baseline"), 0.2150);
            c.insert(QStringLiteral("observed"), 0.2617);
            c.insert(QStringLiteral("required"), 0.2150);
            c.insert(QStringLiteral("met"), false);
            criteria.replace(i, c);
        }
        gate.insert(QStringLiteral("criteria"), criteria);
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), report, gate, ledger, {}, kNow);
        const BotCockpitNode* node = scene.node(QStringLiteral("brier_split"));
        QVERIFY(node != nullptr);
        QVERIFY(node->value.contains(QStringLiteral("model 0.0698")));
        QVERIFY(node->value.contains(QStringLiteral("bot-settled 0.2617")));
        QVERIFY(node->value.contains(QStringLiteral("BOT LOSES TO MID")));
        QCOMPARE(node->role, QStringLiteral("red"));
    }

    // ---- the cockpit must not re-pool what the gate unpooled --------------
    static QJsonObject fam_block(bool passes, int eligible) {
        return QJsonObject{
            {QStringLiteral("adds_value_over_market"), passes},
            {QStringLiteral("brier_full"), 0.05},
            {QStringLiteral("brier_market_mid_raw"), 0.30},
            {QStringLiteral("adds_value_on_bet_eligible"), passes},
            {QStringLiteral("brier_eligible_full"), passes ? 0.05 : 0.40},
            {QStringLiteral("brier_eligible_market_mid_raw"), 0.30},
            {QStringLiteral("eligible_scored_contracts"), eligible},
            {QStringLiteral("min_eligible_contracts"), 100},
        };
    }

    // Gold must never look green because Silver carried the pool.
    void one_passing_family_never_greens_the_group() {
        const QJsonObject report{
            {QStringLiteral("by_family"),
             QJsonObject{{QStringLiteral("KXGOLDH"), fam_block(true, 200)},
                         {QStringLiteral("KXSILVERH"), fam_block(false, 200)}}}};
        const auto states = family_states(report);
        QCOMPARE(states.size(), 2);
        // A measured loss outranks everything: the group cannot read green.
        QCOMPARE(family_group_colour(states), QStringLiteral("amber"));
        const QString line = family_states_line(states);
        QVERIFY(line.contains(QStringLiteral("KXGOLDH T")));
        QVERIFY(line.contains(QStringLiteral("KXSILVERH U")));
    }

    // The footgun after the split: every family starts at zero, and rendering
    // "not measured" as "measured and lost" invites dropping instruments
    // nobody has looked at. Unmeasured is GREY, never amber.
    void an_unmeasured_family_is_grey_not_amber() {
        const QJsonObject report{
            {QStringLiteral("by_family"),
             QJsonObject{{QStringLiteral("KXGOLDH"), fam_block(false, 0)},
                         {QStringLiteral("KXSILVERH"), fam_block(false, 0)},
                         {QStringLiteral("KXWTIH"), fam_block(false, 0)}}}};
        const auto states = family_states(report);
        QCOMPARE(family_group_colour(states), QStringLiteral("grey"));
        QVERIFY(family_states_line(states).contains(QStringLiteral("KXWTIH ?")));
    }

    // Green requires EVERY family to pass on its own evidence.
    void green_requires_every_family_to_pass() {
        const QJsonObject all_pass{
            {QStringLiteral("by_family"),
             QJsonObject{{QStringLiteral("KXGOLDH"), fam_block(true, 200)},
                         {QStringLiteral("KXSILVERH"), fam_block(true, 200)}}}};
        QCOMPARE(family_group_colour(family_states(all_pass)),
                 QStringLiteral("green"));

        // One unmeasured family is enough to withhold green.
        const QJsonObject mixed{
            {QStringLiteral("by_family"),
             QJsonObject{{QStringLiteral("KXGOLDH"), fam_block(true, 200)},
                         {QStringLiteral("KXSILVERH"), fam_block(false, 0)}}}};
        QCOMPARE(family_group_colour(family_states(mixed)),
                 QStringLiteral("grey"));
    }

    // ── DECIDE family chips + gate by_family strip (display-only mirror) ───

    void decide_family_chips_are_orbit_buckets_not_global_newest() {
        // Gold PASS + BTC15 BID → GOLD PASS / 15M BID; THR stays "—".
        // A single global-newest fill would incorrectly paint every chip BID.
        const QJsonArray ledger{
            decision_row(kNow - 20'000, QStringLiteral("KXGOLDH-26AUG1112-T3400"),
                         QStringLiteral("EDGE_BELOW_THRESHOLD")),
            bid_row(kNow - 5'000, QStringLiteral("KXBTC15M-26AUG111215-B64000"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        QCOMPARE(scene.family_chips.size(), 6);
        QCOMPARE(scene.family_chips.at(0).id, QStringLiteral("thr"));
        QCOMPARE(scene.family_chips.at(0).action, QStringLiteral("—"));
        QCOMPARE(scene.family_chips.at(0).role, QStringLiteral("grey"));
        const BotCockpitFamilyChip* btc15 = scene.family_chip(QStringLiteral("btc15"));
        QVERIFY(btc15 != nullptr);
        QCOMPARE(btc15->action, QStringLiteral("BID"));
        QCOMPARE(btc15->role, QStringLiteral("green"));
        QVERIFY(btc15->ticker.contains(QStringLiteral("KXBTC15M")));
        const BotCockpitFamilyChip* gold = scene.family_chip(QStringLiteral("gold"));
        QVERIFY(gold != nullptr);
        QCOMPARE(gold->action, QStringLiteral("PASS"));
        QCOMPARE(gold->role, QStringLiteral("cyan"));
        QVERIFY(gold->ticker.contains(QStringLiteral("KXGOLDH")));
        // Envelope still tracks newest overall (the BTC15 bid).
        QVERIFY(scene.envelope.startsWith(QStringLiteral("BID")));
        QVERIFY(scene.envelope_ticker.contains(QStringLiteral("KXBTC15M")));
    }

    void one_metal_bid_does_not_paint_sibling_metals() {
        const QJsonArray ledger{
            bid_row(kNow - 3'000, QStringLiteral("KXGOLDH-26AUG1112-T3400"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger), {}, {}, ledger, {}, kNow);
        QCOMPARE(scene.family_chip(QStringLiteral("gold"))->action, QStringLiteral("BID"));
        QCOMPARE(scene.family_chip(QStringLiteral("silver"))->action, QStringLiteral("—"));
        QCOMPARE(scene.family_chip(QStringLiteral("wti"))->action, QStringLiteral("—"));
        QCOMPARE(scene.family_chip(QStringLiteral("silver"))->role, QStringLiteral("grey"));
        QCOMPARE(scene.family_chip(QStringLiteral("wti"))->role, QStringLiteral("grey"));
    }

    void sealed_gate_states_no_families_preregistered_explicitly() {
        QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55);
        gate.insert(QStringLiteral("by_family_eligible"), false);
        gate.insert(QStringLiteral("by_family"), QJsonObject());
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KXBTCD-26AUG1112-T64000"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
        QVERIFY(scene.gate_family_line.contains(QStringLiteral("no families preregistered")));
        const BotCockpitNode* node = scene.node(QStringLiteral("gate"));
        QVERIFY(node != nullptr);
        QVERIFY2(node->value.contains(QStringLiteral("no families preregistered")),
                 qPrintable(node->value));
        QVERIFY(node->value.startsWith(QStringLiteral("FAIL")));
    }

    void sealed_gate_family_strip_lists_each_preregistered_verdict() {
        QJsonObject gate = evaluated_gate(QStringLiteral("FAIL"), 4, 3, 1, 1.35, 1.55);
        gate.insert(QStringLiteral("by_family_eligible"), true);
        gate.insert(QStringLiteral("by_family"),
                    QJsonObject{{QStringLiteral("KXGOLDH"),
                                 QJsonObject{{QStringLiteral("verdict"), QStringLiteral("FAIL")}}},
                                {QStringLiteral("KXBTC15M"),
                                 QJsonObject{{QStringLiteral("verdict"), QStringLiteral("PASS")}}}});
        const QJsonArray ledger{decision_row(kNow - 5'000, QStringLiteral("KXBTCD-26AUG1112-T64000"),
                                             QStringLiteral("EDGE_BELOW_THRESHOLD"))};
        const BotCockpitScene scene =
            present_bot_cockpit(panel_for(ledger, gate), {}, gate, ledger, {}, kNow);
        QVERIFY(scene.gate_family_line.contains(QStringLiteral("KXGOLDH FAIL")));
        QVERIFY(scene.gate_family_line.contains(QStringLiteral("KXBTC15M PASS")));
        // Pooled top-level verdict stays the headline; strip is additive.
        QVERIFY(scene.node(QStringLiteral("gate"))->value.startsWith(QStringLiteral("FAIL")));
        QVERIFY(scene.node(QStringLiteral("gate"))
                    ->value.contains(QStringLiteral("KXGOLDH FAIL")));
    }

    void corridor_watch_and_gate_are_visible_and_strategy_separate() {
        const QJsonObject scan{
            {"event", "kalshi_btc_threshold_corridor_scan"},
            {"family", "btc_threshold_corridor"},
            {"certificate", QJsonObject{{"event_ticker", "KXBTCD-26AUG1314"}}},
            {"certificate_sha256", "reviewed-digest"},
            {"evaluation", QJsonObject{{"state", "opportunity"},
                                        {"reason", "certified corridor clears"},
                                        {"pairs_evaluated", 6},
                                        {"opportunities", 1},
                                        {"pairs", QJsonArray{QJsonObject{{"evaluation", QJsonObject{
                                            {"state", "opportunity"},
                                            {"net_edge_per_bundle", "0.031"}}}}}}}}};
        const QJsonObject corridor_gate{{"strategy_family", "btc_threshold_corridor"},
                                        {"authority", "paper_only"},
                                        {"paper_bids_authorized", true},
                                        {"live_orders_authorized", false},
                                        {"evaluated", true},
                                        {"verdict", "PASS"},
                                        {"params", QJsonObject{{"max_bundles_per_opportunity", 2},
                                                                 {"max_cost_per_opportunity_usd", 2.0},
                                                                 {"max_scan_age_ms", 60000}}},
                                        {"evidence", QJsonObject{{"scans", 300},
                                                                  {"distinct_events", 3},
                                                                  {"opportunity_scans", 10}}}};
        const BotCockpitScene scene = present_bot_cockpit(
            panel_for({}), {}, {}, {}, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, {}, {}, {}, {}, {}, {}, {}, {}, scan, corridor_gate, 1);
        QCOMPARE(scene.node(QStringLiteral("btc_corridor"))->role, QStringLiteral("green"));
        QVERIFY(scene.node(QStringLiteral("btc_corridor"))->value.contains(QStringLiteral("PAPER OPPORTUNITY")));
        QVERIFY(scene.node(QStringLiteral("btc_corridor"))->value.contains(QStringLiteral("$0.0310")));
        QCOMPARE(scene.node(QStringLiteral("corridor_gate"))->role, QStringLiteral("green"));
        QVERIFY(scene.node(QStringLiteral("corridor_gate"))->value.contains(QStringLiteral("COLLECTION ARMED")));
        QVERIFY(scene.node(QStringLiteral("corridor_gate"))->value.contains(QStringLiteral("PAPER ONLY")));
        QVERIFY(scene.node(QStringLiteral("corridor_gate"))->value.contains(QStringLiteral("LIVE NEVER")));
        QCOMPARE(scene.node(QStringLiteral("corridor_paper"))->role, QStringLiteral("green"));
        QVERIFY(scene.node(QStringLiteral("corridor_paper"))->value.contains(QStringLiteral("1 simulated")));

        // A directional PASS is not a corridor PASS, even though both inspect
        // KXBTCD contracts.
        const QJsonObject directional{{"strategy_family", "KXBTCD"},
                                      {"authority", "live_if_armed"},
                                      {"evaluated", true},
                                      {"verdict", "PASS"}};
        const BotCockpitScene separated = present_bot_cockpit(
            panel_for({}, directional), {}, directional, {}, {}, kNow, QByteArray(),
            kBotCockpitMaxColumns, kBotCockpitMaxPulses, {}, {}, {}, {}, {}, {}, {}, {}, scan,
            directional);
        QCOMPARE(separated.node(QStringLiteral("corridor_gate"))->role, QStringLiteral("grey"));

        const BotCockpitScene no_input = present_bot_cockpit(
            panel_for({}), {}, {}, {}, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, {}, {}, {}, {}, {}, {}, {}, {}, {}, corridor_gate);
        QCOMPARE(no_input.node(QStringLiteral("btc_corridor"))->role, QStringLiteral("grey"));
        QVERIFY(no_input.node(QStringLiteral("btc_corridor"))->value.contains(QStringLiteral("NO INPUT")));
        QVERIFY(no_input.node(QStringLiteral("btc_corridor"))->value.contains(QStringLiteral("NOT a negative result")));

        QJsonObject no_edge = scan;
        QJsonObject no_edge_evaluation = no_edge.value("evaluation").toObject();
        no_edge_evaluation["state"] = QStringLiteral("not_profitable");
        no_edge_evaluation["opportunities"] = 0;
        no_edge["evaluation"] = no_edge_evaluation;
        const BotCockpitScene measured_negative = present_bot_cockpit(
            panel_for({}), {}, {}, {}, {}, kNow, QByteArray(), kBotCockpitMaxColumns,
            kBotCockpitMaxPulses, {}, {}, {}, {}, {}, {}, {}, {}, no_edge, corridor_gate);
        QCOMPARE(measured_negative.node(QStringLiteral("btc_corridor"))->role,
                 QStringLiteral("amber"));
        QVERIFY(measured_negative.node(QStringLiteral("btc_corridor"))->value.contains(
            QStringLiteral("SCANNING / NO EDGE")));
    }

};

QTEST_GUILESS_MAIN(KalshiBotCockpitTest)
#include "tst_kalshi_bot_cockpit.moc"
