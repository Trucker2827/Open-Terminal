#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotOrders;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotStopFile;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_is_replay_event;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_read_ledger;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;

/// One calibrator.json prediction in spot_calibrator.py build_report() shape.
QJsonObject prediction(double p_full, double market_mid, double minutes_left) {
    return QJsonObject{
        {QStringLiteral("p_yes_full"), p_full},
        {QStringLiteral("p_yes_market_baseline"), market_mid},
        {QStringLiteral("market_yes_mid"), market_mid},
        {QStringLiteral("features"),
         QJsonObject{{QStringLiteral("signed_distance_bps"), 120.0},
                     {QStringLiteral("per_min_vol_bps"), 8.0},
                     {QStringLiteral("sqrt_minutes_left"), std::sqrt(minutes_left)},
                     {QStringLiteral("required_move_sigma"), 1.5},
                     {QStringLiteral("realized_move_bps"), 0.0},
                     {QStringLiteral("yes_mid"), market_mid}}},
    };
}

/// A trusted report (adds_value_over_market true, past the calibrator's own
/// ≥100-CONTRACT gate) carrying one contract. Schema 2 (issue #171): the Brier
/// is scored per contract, `scored_contracts` is its denominator, and the
/// claim is measured against the RAW MID, not the trained logit baseline.
///
/// `trusted` also drives the bet-eligible-subset fields added on top of the
/// full-population claim: a report that wins where the bot bets (`trusted`)
/// carries the same shape a genuinely trusted calibrator report would, and one
/// that does not is untrusted on both counts, never just the population one.
/// Tests that exercise the eligible-subset rule itself build their own
/// QJsonObjects rather than going through this helper.
QJsonObject report(double p_full, double market_mid, double minutes_left,
                   bool trusted = true, qint64 generated_ms = kNow) {
    return QJsonObject{
        {QStringLiteral("schema"), 2},
        {QStringLiteral("event"), QStringLiteral("spot_calibrator")},
        {QStringLiteral("advisory_only"), true},
        {QStringLiteral("generated_at_ms"), static_cast<double>(generated_ms)},
        {QStringLiteral("resolved_contracts"), 371},
        {QStringLiteral("scored_contracts"), 244},
        {QStringLiteral("training_observations"), 12'049},
        {QStringLiteral("min_scored_contracts"), 100},
        {QStringLiteral("brier_full"), 0.1079},
        {QStringLiteral("brier_market_mid_raw"), 0.1083},
        {QStringLiteral("brier_market_trained_logit"), 0.1101},
        {QStringLiteral("adds_value_over_market"), trusted},
        {QStringLiteral("beats_trained_logit_baseline"), true},
        {QStringLiteral("adds_value_on_bet_eligible"), trusted},
        {QStringLiteral("brier_eligible_full"), trusted ? 0.2130 : 0.2576},
        {QStringLiteral("brier_eligible_market_mid_raw"), trusted ? 0.2576 : 0.2130},
        {QStringLiteral("predictions"),
         QJsonObject{{QStringLiteral("KXBTC15M-26JUL241015-15"),
                      prediction(p_full, market_mid, minutes_left)}}},
    };
}

/// The same report with the daemon's observed top-of-book passed through, as
/// spot_calibrator.py's extract_book() writes it. A side the book does not
/// quote is simply not among the keys — absent, never zero.
QJsonObject with_book(QJsonObject full, const QJsonObject& book) {
    QJsonObject predictions = full.value(QStringLiteral("predictions")).toObject();
    const QString ticker = predictions.keys().first();
    QJsonObject entry = predictions.value(ticker).toObject();
    for (auto it = book.constBegin(); it != book.constEnd(); ++it)
        entry.insert(it.key(), it.value());
    predictions.insert(ticker, entry);
    full.insert(QStringLiteral("predictions"), predictions);
    return full;
}

/// Merge keys into the single prediction's `features` object.
QJsonObject with_features(QJsonObject full, const QJsonObject& feature_patch) {
    QJsonObject predictions = full.value(QStringLiteral("predictions")).toObject();
    const QString ticker = predictions.keys().first();
    QJsonObject entry = predictions.value(ticker).toObject();
    QJsonObject features = entry.value(QStringLiteral("features")).toObject();
    for (auto it = feature_patch.constBegin(); it != feature_patch.constEnd(); ++it)
        features.insert(it.key(), it.value());
    entry.insert(QStringLiteral("features"), features);
    predictions.insert(ticker, entry);
    full.insert(QStringLiteral("predictions"), predictions);
    return full;
}

QJsonObject only_row(const QJsonArray& rows) {
    return rows.isEmpty() ? QJsonObject() : rows.first().toObject();
}

QString reason(const QJsonArray& rows) {
    return only_row(rows).value(QStringLiteral("reason_code")).toString();
}

QString action(const QJsonArray& rows) {
    return only_row(rows).value(QStringLiteral("action")).toString();
}

// A filled paper position as KalshiBotOrders::replay hands it to settle_paper.
QJsonObject open_position(const QString& ticker, qint64 placed_ms) {
    return QJsonObject{
        {QStringLiteral("action"), QStringLiteral("bid")},
        {QStringLiteral("ticker"), ticker},
        {QStringLiteral("side"), QStringLiteral("YES")},
        {QStringLiteral("contracts"), 3},
        {QStringLiteral("price"), 0.63},
        {QStringLiteral("stake_usd"), 1.89},
        {QStringLiteral("fee_usd"), 0.02},
        {QStringLiteral("ts_ms"), static_cast<double>(placed_ms)},
        {QStringLiteral("position_id"), ticker + QStringLiteral("@") + QString::number(placed_ms)}};
}
constexpr qint64 k49h = 49LL * 60 * 60 * 1000;
constexpr qint64 k1h = 60LL * 60 * 1000;

} // namespace

class TestKalshiBotDecision : public QObject {
    Q_OBJECT

  private slots:
    // --- the kill switch (rung 4) ----------------------------------------

    /// The report used here is the SAME one that bids in
    /// edge_below_threshold_passes_and_above_bids — the positive control is
    /// asserted in the same test, so a passing result cannot come from a
    /// report that would not have bid anyway.
    void an_engaged_kill_switch_refuses_the_bid_it_would_otherwise_place() {
        const QJsonObject bidding_report = report(0.98, 0.83, 10.0);
        const QJsonArray without_stop = KalshiBotDecision::decide(bidding_report, {}, {}, kNow, {});
        QCOMPARE(action(without_stop), QStringLiteral("bid"));  // positive control

        KalshiBotStopFile stop;
        stop.engaged = true;
        stop.ts_ms = kNow - 5'000;
        stop.source = QStringLiteral("cli");
        stop.reason = QStringLiteral("operator pulled it");
        const QJsonArray rows =
            KalshiBotDecision::decide(bidding_report, {}, {}, kNow, {}, stop);

        QCOMPARE(rows.size(), 1);
        QCOMPARE(action(rows), QStringLiteral("pass"));
        QCOMPARE(reason(rows), QStringLiteral("BOT_STOPPED"));
        // Not one bid row anywhere in the tick.
        for (const auto& value : rows)
            QVERIFY(value.toObject().value(QStringLiteral("action")).toString() !=
                    QStringLiteral("bid"));
        // Who threw it and when, on the audit row itself.
        const QJsonObject row = only_row(rows);
        QCOMPARE(row.value(QStringLiteral("stop_source")).toString(), QStringLiteral("cli"));
        QCOMPARE(row.value(QStringLiteral("stop_reason")).toString(),
                 QStringLiteral("operator pulled it"));
        QCOMPARE(static_cast<qint64>(row.value(QStringLiteral("stop_ts_ms")).toDouble()),
                 kNow - 5'000);
        // The report was never read, so no number from it leaks onto the row.
        QVERIFY(!row.contains(QStringLiteral("calibrated_p")));
        QVERIFY(!row.contains(QStringLiteral("edge")));
        QVERIFY(!row.contains(QStringLiteral("price")));
    }

    /// The stop is journaled, not silent: a tick that refused must be
    /// distinguishable from a loop that simply died.
    void a_stopped_tick_still_journals_exactly_one_row() {
        KalshiBotStopFile stop;
        stop.engaged = true;
        const QJsonArray rows = KalshiBotDecision::decide({}, {}, {}, kNow, {}, stop);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(reason(rows), QStringLiteral("BOT_STOPPED"));
        const QJsonObject row = only_row(rows);
        QCOMPARE(row.value(QStringLiteral("event")).toString(),
                 QStringLiteral("kalshi_bot_decision"));
        QCOMPARE(row.value(QStringLiteral("mode")).toString(), QStringLiteral("paper"));
        QCOMPARE(row.value(QStringLiteral("live_eligible")).toBool(), false);
        // The switch outranks even a missing report: the tick stops before the
        // report is looked at, so it reports the stop, not REPORT_MISSING.
        QVERIFY(reason(rows) != QStringLiteral("REPORT_MISSING"));
    }

    // --- refusals: the file itself is not trustworthy -------------------

    void missing_report_is_refused_and_journaled() {
        const QJsonArray rows = KalshiBotDecision::decide({}, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(reason(rows), QStringLiteral("REPORT_MISSING"));
        QCOMPARE(action(rows), QStringLiteral("pass"));
        // A refused report carries no numbers at all — a withheld probability
        // must never arrive as a default-constructed 0.0.
        const QJsonObject row = only_row(rows);
        QVERIFY(!row.contains(QStringLiteral("calibrated_p")));
        QVERIFY(!row.contains(QStringLiteral("edge")));
        QVERIFY(!row.contains(QStringLiteral("price")));
    }

    void report_at_or_past_120s_is_refused() {
        KalshiBotDecision::Config config;
        QCOMPARE(config.max_report_age_ms, 120'000LL);
        // 119.9s old: still fresh, the contract is decided normally.
        const QJsonArray fresh =
            KalshiBotDecision::decide(report(0.98, 0.83, 10.0, true, kNow - 119'900),
                                      {}, {}, kNow, config);
        QCOMPARE(fresh.size(), 1);
        QCOMPARE(action(fresh), QStringLiteral("bid"));
        // Exactly 120s old: refused.
        const QJsonArray stale =
            KalshiBotDecision::decide(report(0.98, 0.83, 10.0, true, kNow - 120'000),
                                      {}, {}, kNow, config);
        QCOMPARE(stale.size(), 1);
        QCOMPARE(reason(stale), QStringLiteral("REPORT_STALE"));
        QCOMPARE(action(stale), QStringLiteral("pass"));
        QVERIFY(!only_row(stale).contains(QStringLiteral("calibrated_p")));
    }

    // --- the edge threshold ---------------------------------------------

    void edge_below_threshold_passes_and_above_bids() {
        KalshiBotDecision::Config config;
        config.edge_threshold = 0.10;
        // |0.90 − 0.83| = 0.07 < 0.10 → pass, but the pass is journaled WITH
        // the numbers that produced it.
        const QJsonArray thin = KalshiBotDecision::decide(report(0.90, 0.83, 10.0), {}, {}, kNow, config);
        QCOMPARE(reason(thin), QStringLiteral("EDGE_BELOW_THRESHOLD"));
        QCOMPARE(action(thin), QStringLiteral("pass"));
        QCOMPARE(only_row(thin).value(QStringLiteral("edge")).toDouble(), 0.07);
        QVERIFY(!only_row(thin).contains(QStringLiteral("price")));

        // |0.98 − 0.83| = 0.15 clears the base threshold AND the resting
        // tier's premium on top of it (#165) → YES bid.
        const QJsonArray fat = KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, config);
        QCOMPARE(action(fat), QStringLiteral("bid"));
        QCOMPARE(reason(fat), QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        QCOMPARE(only_row(fat).value(QStringLiteral("side")).toString(), QStringLiteral("YES"));

        // A calibrated probability BELOW the mid takes the NO side, priced
        // off the complement.
        const QJsonArray short_side =
            KalshiBotDecision::decide(report(0.60, 0.83, 10.0), {}, {}, kNow, config);
        QCOMPARE(action(short_side), QStringLiteral("bid"));
        QCOMPARE(only_row(short_side).value(QStringLiteral("side")).toString(), QStringLiteral("NO"));
        QCOMPARE(only_row(short_side).value(QStringLiteral("price")).toDouble(), 0.17);
    }

    // --- two-tier quoting: pay to fill (issue #158) -----------------------

    /// (a) An edge that does NOT clear the spread + fee + margin quotes
    ///     exactly where it quoted before this rung existed: floor(mid).
    ///     The no-book report is the control — the two prices must agree.
    void an_edge_below_the_crossing_cost_rests_exactly_as_before() {
        KalshiBotDecision::Config config;
        // The resting tier's own premium (#165) is not what this test is
        // about — zeroed so the CROSSING arithmetic alone decides, exactly as
        // this test was written for #158. Its premium is proved separately in
        // a_rest_must_clear_the_premium_and_a_cross_need_not.
        config.rest_premium_usd = 0.0;
        // |0.95 − 0.83| = 0.12 clears the edge threshold, so this contract
        // reaches pricing; a 10-cent ask spread is what it cannot clear.
        const QJsonArray control =
            KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, config);
        const QJsonArray wide = KalshiBotDecision::decide(
            with_book(report(0.95, 0.83, 10.0), {{QStringLiteral("market_yes_bid"), 0.82},
                                                 {QStringLiteral("market_yes_ask"), 0.93}}),
            {}, {}, kNow, config);

        QCOMPARE(action(wide), QStringLiteral("bid"));
        QCOMPARE(reason(wide), QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        QCOMPARE(only_row(wide).value(QStringLiteral("quote_style")).toString(),
                 QStringLiteral("rest"));
        QCOMPARE(only_row(wide).value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_EDGE_BELOW_COST"));
        // The price is rung 1's, to the cent, and identical to the control's.
        QCOMPARE(only_row(wide).value(QStringLiteral("price")).toDouble(), 0.83);
        QCOMPARE(only_row(wide).value(QStringLiteral("price")).toDouble(),
                 only_row(control).value(QStringLiteral("price")).toDouble());
        QCOMPARE(only_row(wide).value(QStringLiteral("limit_price")).toDouble(), 0.83);
        // 0.12 of edge against 0.10 of spread + 0.01 of fee + 0.02 of margin.
        QCOMPARE(only_row(wide).value(QStringLiteral("side_edge")).toDouble(), 0.12);
        QCOMPARE(only_row(wide).value(QStringLiteral("spread_cost_usd")).toDouble(), 0.10);
        QCOMPARE(only_row(wide).value(QStringLiteral("cross_fee_usd")).toDouble(), 0.01);
        QCOMPARE(only_row(wide).value(QStringLiteral("cross_margin_usd")).toDouble(), 0.02);
        QVERIFY(only_row(wide).value(QStringLiteral("net_ev_usd")).toDouble() <= 0.02);
    }

    /// (b) An edge that DOES clear it crosses: the bid is quoted at the ask,
    ///     and the row carries the arithmetic that justified paying for it.
    void an_edge_above_the_crossing_cost_quotes_at_the_ask() {
        KalshiBotDecision::Config config;
        const QJsonArray rows = KalshiBotDecision::decide(
            with_book(report(0.95, 0.83, 10.0), {{QStringLiteral("market_yes_bid"), 0.82},
                                                 {QStringLiteral("market_yes_ask"), 0.84}}),
            {}, {}, kNow, config);
        const QJsonObject row = only_row(rows);

        QCOMPARE(action(rows), QStringLiteral("bid"));
        QCOMPARE(row.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(row.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("CROSS_EDGE_CLEARS_COST"));
        QCOMPARE(row.value(QStringLiteral("side")).toString(), QStringLiteral("YES"));
        // AT the ask, not at the mid: 0.84, where resting would have been 0.83.
        QCOMPARE(row.value(QStringLiteral("price")).toDouble(), 0.84);
        QCOMPARE(row.value(QStringLiteral("limit_price")).toDouble(), 0.84);
        QCOMPARE(row.value(QStringLiteral("rest_price")).toDouble(), 0.83);
        QCOMPARE(row.value(QStringLiteral("side_ask")).toDouble(), 0.84);
        QCOMPARE(row.value(QStringLiteral("side_bid")).toDouble(), 0.82);
        // Both forms of the same inequality, net of BOTH costs.
        QCOMPARE(row.value(QStringLiteral("spread_cost_usd")).toDouble(), 0.01);
        QCOMPARE(row.value(QStringLiteral("cross_fee_usd")).toDouble(), 0.01);
        QCOMPARE(row.value(QStringLiteral("cross_cost_usd")).toDouble(), 0.02);
        QCOMPARE(row.value(QStringLiteral("net_ev_usd")).toDouble(), 0.10);
        QVERIFY(row.value(QStringLiteral("side_edge")).toDouble() >
                row.value(QStringLiteral("cross_cost_usd")).toDouble() +
                    row.value(QStringLiteral("cross_margin_usd")).toDouble());
        // The order still starts life resting under the SAME stated model —
        // no self-flattery: nothing here fills anything.
        QCOMPARE(row.value(QStringLiteral("order_state")).toString(), QStringLiteral("resting"));
        QCOMPARE(row.value(QStringLiteral("fill_model")).toString(),
                 QString::fromLatin1(KalshiBotOrders::kFillModel));
        // The model's NAME, and not the prose beside it: KalshiBotOrders'
        // fill_rule describes the passive tier ("calibrator.json carries no
        // book", "its market mid is the ask proxy", "it selects on the market
        // having moved to the quote") and every clause of that is false of a
        // crossing bid. A false disclosure is worse than none.
        QVERIFY(!row.contains(QStringLiteral("fill_rule")));
    }

    /// Postmortem lesson: refuse NO fades of already-high YES with short runway.
    void no_fade_of_high_yes_near_close_is_refused() {
        KalshiBotDecision::Config config;
        // YES mid 0.90, model 0.70 → NO; 5 minutes left ≤ 10m ban window.
        const QJsonArray banned = KalshiBotDecision::decide(
            with_book(report(0.70, 0.90, 5.0), {{QStringLiteral("market_no_bid"), 0.08},
                                                {QStringLiteral("market_no_ask"), 0.10}}),
            {}, {}, kNow, config);
        QCOMPARE(action(banned), QStringLiteral("pass"));
        QCOMPARE(reason(banned), QString::fromLatin1(KalshiBotDecision::kFadeYesNearClose));
        QCOMPARE(only_row(banned).value(QStringLiteral("side")).toString(), QStringLiteral("NO"));

        // Same fade with 20m runway clears the ban window and may still bid.
        const QJsonArray allowed = KalshiBotDecision::decide(
            with_book(report(0.70, 0.90, 20.0), {{QStringLiteral("market_no_bid"), 0.08},
                                                 {QStringLiteral("market_no_ask"), 0.10}}),
            {}, {}, kNow, config);
        QCOMPARE(action(allowed), QStringLiteral("bid"));
        QCOMPARE(only_row(allowed).value(QStringLiteral("side")).toString(), QStringLiteral("NO"));
    }

    /// Fade ban lifts only when venue lead confirms down (fail closed otherwise).
    void no_fade_ban_lifts_only_when_lead_confirms_down() {
        KalshiBotDecision::Config config;
        const auto no_book = QJsonObject{{QStringLiteral("market_no_bid"), 0.08},
                                         {QStringLiteral("market_no_ask"), 0.10}};
        const QJsonObject base = with_book(report(0.70, 0.90, 5.0), no_book);

        // Conflict or missing lead → ban stands.
        const QJsonArray conflicted = KalshiBotDecision::decide(
            with_features(base,
                          {{QStringLiteral("lead_confirms_direction"), true},
                           {QStringLiteral("lead_conflicts"), true},
                           {QStringLiteral("venue_lead_bps_30s"), -12.0}}),
            {}, {}, kNow, config);
        QCOMPARE(reason(conflicted), QString::fromLatin1(KalshiBotDecision::kFadeYesNearClose));

        const QJsonArray upward = KalshiBotDecision::decide(
            with_features(base,
                          {{QStringLiteral("lead_confirms_direction"), true},
                           {QStringLiteral("lead_conflicts"), false},
                           {QStringLiteral("venue_lead_bps_30s"), 8.0}}),
            {}, {}, kNow, config);
        QCOMPARE(reason(upward), QString::fromLatin1(KalshiBotDecision::kFadeYesNearClose));

        // Confirm down → ban lifts; journal marks the lift.
        const QJsonArray lifted = KalshiBotDecision::decide(
            with_features(base,
                          {{QStringLiteral("lead_confirms_direction"), true},
                           {QStringLiteral("lead_conflicts"), false},
                           {QStringLiteral("venue_lead_bps_30s"), -15.0}}),
            {}, {}, kNow, config);
        QCOMPARE(action(lifted), QStringLiteral("bid"));
        QCOMPARE(only_row(lifted).value(QStringLiteral("side")).toString(), QStringLiteral("NO"));
        QVERIFY(only_row(lifted).value(QStringLiteral("fade_ban_lifted")).toBool());
        QCOMPARE(only_row(lifted).value(QStringLiteral("fade_ban_lift_reason")).toString(),
                 QStringLiteral("lead_confirms_down"));

        // BRTI avg60 below open (and p_brti < 0.5) also lifts without lead.
        const QJsonArray brti_lifted = KalshiBotDecision::decide(
            with_features(base,
                          {{QStringLiteral("open_price"), 100000.0},
                           {QStringLiteral("brti_avg_60s"), 99900.0},
                           {QStringLiteral("p_brti_avg60"), 0.35}}),
            {}, {}, kNow, config);
        QCOMPARE(action(brti_lifted), QStringLiteral("bid"));
        QCOMPARE(only_row(brti_lifted).value(QStringLiteral("fade_ban_lift_reason")).toString(),
                 QStringLiteral("brti_avg60_below_open"));

        // BRTI below open but physics still ≥0.5 → ban stands.
        const QJsonArray brti_high_p = KalshiBotDecision::decide(
            with_features(base,
                          {{QStringLiteral("open_price"), 100000.0},
                           {QStringLiteral("brti_avg_60s"), 99900.0},
                           {QStringLiteral("p_brti_avg60"), 0.55}}),
            {}, {}, kNow, config);
        QCOMPARE(reason(brti_high_p), QString::fromLatin1(KalshiBotDecision::kFadeYesNearClose));
    }

    void mid_path_samples_cap_and_attach_onto_settlement() {
        KalshiBotDecision::MidPathStore store;
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        const QJsonObject position = open_position(ticker, kNow - 60'000);
        const QString pid = position.value(QStringLiteral("position_id")).toString();
        QJsonObject predictions{
            {ticker, QJsonObject{{QStringLiteral("market_yes_mid"), 0.70},
                                 {QStringLiteral("market_yes_bid"), 0.69}}}};
        KalshiBotDecision::sample_mid_path(QJsonArray{position}, predictions, kNow, &store);
        predictions.insert(ticker,
                           QJsonObject{{QStringLiteral("market_yes_mid"), 0.72},
                                       {QStringLiteral("market_yes_bid"), 0.71}});
        KalshiBotDecision::sample_mid_path(
            QJsonArray{position}, predictions, kNow + 1'000, &store);
        // Same tick twice → still one sample for that ts.
        KalshiBotDecision::sample_mid_path(
            QJsonArray{position}, predictions, kNow + 1'000, &store);
        QCOMPARE(store.value(pid).size(), 2);

        QJsonObject settlement{{QStringLiteral("position_id"), pid},
                               {QStringLiteral("ticker"), ticker}};
        KalshiBotDecision::attach_mid_path(settlement, &store);
        QCOMPARE(settlement.value(QStringLiteral("mid_path")).toArray().size(), 2);
        QVERIFY(!store.contains(pid));
        QCOMPARE(settlement.value(QStringLiteral("mid_path"))
                     .toArray()
                     .last()
                     .toObject()
                     .value(QStringLiteral("market_yes_mid"))
                     .toDouble(),
                 0.72);
    }

    /// Postmortem lesson: crossing a favourite ask needs extra surviving EV.
    void favourite_cross_requires_extra_margin_else_rests() {
        KalshiBotDecision::Config config;
        config.rest_premium_usd = 0.0;  // isolate the cross hurdle
        // net_ev = 0.76 − 0.70 − 0.02fee = 0.04 clears base margin 0.02 but
        // not favourite margin 0.05 → must REST, not cross.
        const QJsonArray rows = KalshiBotDecision::decide(
            with_book(report(0.76, 0.63, 10.0), {{QStringLiteral("market_yes_bid"), 0.62},
                                                 {QStringLiteral("market_yes_ask"), 0.70}}),
            {}, {}, kNow, config);
        QCOMPARE(action(rows), QStringLiteral("bid"));
        QCOMPARE(only_row(rows).value(QStringLiteral("quote_style")).toString(),
                 QStringLiteral("rest"));
        QCOMPARE(only_row(rows).value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_EDGE_BELOW_COST"));
        QCOMPARE(only_row(rows).value(QStringLiteral("effective_cross_margin_usd")).toDouble(),
                 0.05);
    }

    /// A NO bid crosses at the NO book's OWN ask. Kalshi's NO book is a book,
    /// not `1 − yes_bid`; the fixture's two books are deliberately not mirrors
    /// so an inference from the YES side would produce a different price.
    void a_no_side_cross_uses_the_no_books_own_ask() {
        KalshiBotDecision::Config config;
        const QJsonArray rows = KalshiBotDecision::decide(
            with_book(report(0.20, 0.60, 10.0), {{QStringLiteral("market_yes_bid"), 0.59},
                                                 {QStringLiteral("market_yes_ask"), 0.61},
                                                 {QStringLiteral("market_no_bid"), 0.39},
                                                 {QStringLiteral("market_no_ask"), 0.42}}),
            {}, {}, kNow, config);
        const QJsonObject row = only_row(rows);

        QCOMPARE(row.value(QStringLiteral("side")).toString(), QStringLiteral("NO"));
        QCOMPARE(row.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(row.value(QStringLiteral("price")).toDouble(), 0.42);   // the NO ask
        QVERIFY(row.value(QStringLiteral("price")).toDouble() != 0.41);  // NOT 1 − yes_bid
        QCOMPARE(row.value(QStringLiteral("side_bid")).toDouble(), 0.39);
        QCOMPARE(row.value(QStringLiteral("rest_price")).toDouble(), 0.40);
    }

    /// (c) No spread data → rest, always. An unknown spread is not a free one,
    ///     and a book that contradicts its own mid is a species of unknown.
    void a_contract_without_usable_book_data_never_crosses() {
        KalshiBotDecision::Config config;

        // Nothing at all: the report every calibrator build before this rung
        // wrote. It rests, and it invents no arithmetic to explain itself.
        const QJsonObject bare = only_row(
            KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, config));
        QCOMPARE(bare.value(QStringLiteral("quote_style")).toString(), QStringLiteral("rest"));
        QCOMPARE(bare.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_NO_BOOK"));
        QCOMPARE(bare.value(QStringLiteral("price")).toDouble(), 0.83);
        for (const char* absent : {"side_ask", "side_bid", "cross_price", "spread_cost_usd",
                                   "cross_fee_usd", "cross_cost_usd", "net_ev_usd"})
            QVERIFY2(!bare.contains(QLatin1String(absent)), absent);

        // The NO book is missing while the YES book is present: the side being
        // bid is the one that has to be quoted, so this still rests.
        const QJsonObject wrong_side = only_row(KalshiBotDecision::decide(
            with_book(report(0.20, 0.60, 10.0), {{QStringLiteral("market_yes_bid"), 0.59},
                                                 {QStringLiteral("market_yes_ask"), 0.61}}),
            {}, {}, kNow, config));
        QCOMPARE(wrong_side.value(QStringLiteral("side")).toString(), QStringLiteral("NO"));
        QCOMPARE(wrong_side.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_NO_BOOK"));

        // An ask BELOW the mid it is supposed to be half of. Crossing here
        // would price a negative spread cost — a free lunch, fabricated.
        const QJsonObject contradictory = only_row(KalshiBotDecision::decide(
            with_book(report(0.98, 0.83, 10.0), {{QStringLiteral("market_yes_ask"), 0.80}}),
            {}, {}, kNow, config));
        QCOMPARE(contradictory.value(QStringLiteral("quote_style")).toString(),
                 QStringLiteral("rest"));
        QCOMPARE(contradictory.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_BOOK_INCONSISTENT"));
        QCOMPARE(contradictory.value(QStringLiteral("price")).toDouble(), 0.83);
        QVERIFY(!contradictory.contains(QStringLiteral("net_ev_usd")));

        // An ask that rounds up to a whole dollar cannot be paid; the passive
        // tier is still perfectly valid, so the contract rests rather than
        // being dropped as malformed.
        const QJsonObject dollar = only_row(KalshiBotDecision::decide(
            with_book(report(0.99, 0.85, 10.0), {{QStringLiteral("market_yes_ask"), 0.999}}),
            {}, {}, kNow, config));
        QCOMPARE(action(QJsonArray{dollar}), QStringLiteral("bid"));
        QCOMPARE(dollar.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_BOOK_INCONSISTENT"));
        QCOMPARE(dollar.value(QStringLiteral("price")).toDouble(), 0.85);
    }

    /// (d) The fee is part of the hurdle, and a one-cent-quantised fee is
    ///     enough to flip the tier. Margin zeroed so the fee alone decides.
    void the_taker_fee_alone_can_flip_the_tier() {
        KalshiBotDecision::Config config;
        config.cross_margin_usd = 0.0;
        // Both tiers' discretionary hurdles zeroed for the same reason: the
        // FEE is the only thing allowed to decide here.
        config.rest_premium_usd = 0.0;
        const QJsonObject book{{QStringLiteral("market_yes_bid"), 0.30},
                               {QStringLiteral("market_yes_ask"), 0.50}};

        // 0.12 of edge against 0.10 of spread: it clears the SPREAD. The fee
        // at $0.50 is 2¢ — exactly enough to take it back, so it rests.
        const QJsonObject eaten = only_row(KalshiBotDecision::decide(
            with_book(report(0.52, 0.40, 10.0), book), {}, {}, kNow, config));
        QCOMPARE(eaten.value(QStringLiteral("side_edge")).toDouble(), 0.12);
        QCOMPARE(eaten.value(QStringLiteral("spread_cost_usd")).toDouble(), 0.10);
        QCOMPARE(eaten.value(QStringLiteral("cross_fee_usd")).toDouble(), 0.02);
        QVERIFY(eaten.value(QStringLiteral("side_edge")).toDouble() >
                eaten.value(QStringLiteral("spread_cost_usd")).toDouble());   // spread alone: cleared
        QCOMPARE(eaten.value(QStringLiteral("quote_style")).toString(), QStringLiteral("rest"));
        QCOMPARE(eaten.value(QStringLiteral("quote_style_reason")).toString(),
                 QStringLiteral("REST_EDGE_BELOW_COST"));
        QCOMPARE(eaten.value(QStringLiteral("price")).toDouble(), 0.40);

        // One more cent of edge — the only thing changed — and the same
        // contract, the same book and the same fee now clear the hurdle.
        const QJsonObject clears = only_row(KalshiBotDecision::decide(
            with_book(report(0.53, 0.40, 10.0), book), {}, {}, kNow, config));
        QCOMPARE(clears.value(QStringLiteral("cross_fee_usd")).toDouble(), 0.02);
        QCOMPARE(clears.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(clears.value(QStringLiteral("price")).toDouble(), 0.50);
    }

    /// A fractional ask rounds UP to the cent. Paying is a cost, and rounding
    /// a cost down would flatter the hurdle it has to clear.
    void a_fractional_ask_rounds_up_never_down() {
        KalshiBotDecision::Config config;
        const QJsonObject row = only_row(KalshiBotDecision::decide(
            with_book(report(0.95, 0.83, 10.0), {{QStringLiteral("market_yes_ask"), 0.835}}),
            {}, {}, kNow, config));
        QCOMPARE(row.value(QStringLiteral("side_ask")).toDouble(), 0.835);
        QCOMPARE(row.value(QStringLiteral("cross_price")).toDouble(), 0.84);
        QCOMPARE(row.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(row.value(QStringLiteral("price")).toDouble(), 0.84);
    }

    /// A crossing quote is money committed like any other: it reserves budget
    /// against the same caps, and SESSION_BUDGET_BLOCKS_BID still refuses it.
    void a_crossing_bid_reserves_budget_like_a_resting_one() {
        KalshiBotDecision::Config config;
        config.session_budget_usd = 0.50;
        KalshiBotDecision::Exposure exposure;
        exposure.session_opened_usd = 0.40;
        const QJsonObject row = only_row(KalshiBotDecision::decide(
            with_book(report(0.95, 0.83, 10.0), {{QStringLiteral("market_yes_ask"), 0.84}}),
            {}, {}, kNow, config, {}, exposure));
        QCOMPARE(row.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("SESSION_BUDGET_BLOCKS_BID"));
        // The refusal names the tier it was pricing: refused at the ask is a
        // different fact from refused at the mid.
        QCOMPARE(row.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(row.value(QStringLiteral("price")).toDouble(), 0.84);
    }

    // --- Fix 1: the paper loop is not bricked by the lifetime session budget --

    /// With the budget exempted (paper), a bid still fires even though the
    /// session's cumulative all-in is already at the cap. Neuter below proves the
    /// flag is what gates it and live is unchanged.
    void paper_accumulates_past_the_session_budget() {
        const QJsonObject bidding = report(0.98, 0.83, 10.0);

        KalshiBotDecision::Exposure at_cap;                 // cumulative opens at the cap
        at_cap.session_opened_usd = 120.00;

        KalshiBotDecision::Config paper;                    // paper: budget exempt
        paper.enforce_session_budget = false;
        const QJsonArray bids = KalshiBotDecision::decide(bidding, {}, {}, kNow, paper, {}, at_cap);
        QCOMPARE(action(bids), QStringLiteral("bid"));

        // Neuter: default config (live semantics) refuses the very same inputs.
        const QJsonArray blocked = KalshiBotDecision::decide(bidding, {}, {}, kNow, {}, {}, at_cap);
        QCOMPARE(action(blocked), QStringLiteral("pass"));
        QCOMPARE(reason(blocked), QStringLiteral("SESSION_BUDGET_BLOCKS_BID"));
    }

    /// Exempting the session budget must NOT make paper unbounded: the current
    /// open-exposure fence still refuses a bid that would breach it.
    void paper_still_respects_the_open_exposure_cap() {
        const QJsonObject bidding = report(0.98, 0.83, 10.0);

        KalshiBotDecision::Exposure at_open_cap;            // open risk already at the cap
        at_open_cap.at_risk_usd = 120.00;

        KalshiBotDecision::Config paper;
        paper.enforce_session_budget = false;
        const QJsonArray rows = KalshiBotDecision::decide(bidding, {}, {}, kNow, paper, {}, at_open_cap);
        QCOMPARE(action(rows), QStringLiteral("pass"));
        QCOMPARE(reason(rows), QStringLiteral("EXPOSURE_CAP_BLOCKS_BID"));
    }

    // --- sizing ----------------------------------------------------------

    void sizing_respects_the_stake_and_all_in_caps() {
        KalshiBotDecision::Config config;
        config.edge_threshold = 0.10;
        config.max_stake_usd = 2.00;
        config.max_all_in_usd = 3.00;
        // Limit price floors 0.835 → 0.83; 2 contracts cost $1.66, 3 would be
        // $2.49 > $2.00.
        const QJsonObject row = only_row(KalshiBotDecision::decide(report(0.98, 0.835, 10.0), {}, {}, kNow, config));
        QCOMPARE(row.value(QStringLiteral("price")).toDouble(), 0.83);
        QCOMPARE(row.value(QStringLiteral("contracts")).toInt(), 2);
        QCOMPARE(row.value(QStringLiteral("stake_usd")).toDouble(), 1.66);
        QVERIFY(row.value(QStringLiteral("stake_usd")).toDouble() <= config.max_stake_usd);
        QVERIFY(row.value(QStringLiteral("all_in_usd")).toDouble() <= config.max_all_in_usd);
        QCOMPARE(row.value(QStringLiteral("all_in_usd")).toDouble(),
                 row.value(QStringLiteral("stake_usd")).toDouble() +
                     row.value(QStringLiteral("fee_usd")).toDouble());

        // The all-in ceiling BINDS below the stake ceiling: with $0.05 of
        // headroom no size clears stake + fee, so the bot refuses rather than
        // bidding a size it cannot afford.
        config.max_all_in_usd = 0.05;
        const QJsonArray blocked = KalshiBotDecision::decide(report(0.98, 0.835, 10.0), {}, {}, kNow, config);
        QCOMPARE(action(blocked), QStringLiteral("pass"));
        QCOMPARE(reason(blocked), QStringLiteral("SIZE_CAP_BLOCKS_BID"));

        // One contract costing more than the whole stake cap is refused too.
        config.max_all_in_usd = 3.00;
        config.max_stake_usd = 0.50;
        const QJsonArray too_dear = KalshiBotDecision::decide(report(0.98, 0.835, 10.0), {}, {}, kNow, config);
        QCOMPARE(reason(too_dear), QStringLiteral("SIZE_CAP_BLOCKS_BID"));
    }

    // --- runway ----------------------------------------------------------

    void runway_subtracts_time_elapsed_since_the_report_was_generated() {
        KalshiBotDecision::Config config;
        config.edge_threshold = 0.10;
        config.min_runway_seconds = 120;
        // 2.5 minutes (150s) of runway when the report was written, read 100s
        // later: only 50s remain now, so the raw feature would bid and the
        // honest as-of-now runway must not.
        const QJsonArray rows = KalshiBotDecision::decide(
            report(0.95, 0.83, 2.5, true, kNow - 100'000), {}, {}, kNow, config);
        QCOMPARE(reason(rows), QStringLiteral("NO_RUNWAY"));
        QCOMPARE(action(rows), QStringLiteral("pass"));
        QCOMPARE(only_row(rows).value(QStringLiteral("runway_seconds")).toDouble(), 50.0);
    }

    // --- signal trust ----------------------------------------------------

    /// Issue #165, criterion (a): an untrusted signal PASSES. Not a labelled
    /// paper bid — no order at all, however large the edge. The positive
    /// control is the same report with the trust flag flipped, so a passing
    /// result cannot come from a report that would not have bid anyway.
    void an_untrusted_signal_passes_and_puts_no_order_in_the_book() {
        // A 0.60 edge: far past the base threshold and past the resting
        // premium, so nothing but the trust rule can be refusing it.
        const QJsonArray trusted_rows =
            KalshiBotDecision::decide(report(0.95, 0.35, 10.0, true), {}, {}, kNow, {});
        QCOMPARE(action(trusted_rows), QStringLiteral("bid"));  // positive control

        const QJsonArray rows =
            KalshiBotDecision::decide(report(0.95, 0.35, 10.0, false), {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        const QJsonObject untrusted = only_row(rows);
        QCOMPARE(untrusted.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
        QCOMPARE(untrusted.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("SIGNAL_UNTRUSTED"));
        QCOMPARE(untrusted.value(QStringLiteral("signal_trusted")).toBool(), false);
        // Not one bid row anywhere in the tick, and — the criterion as written
        // — nothing in the BOOK either: no resting order, no exposure, no
        // position. The ledger the orders layer replays is the audit surface,
        // so it is the one asserted against.
        for (const auto& value : rows)
            QVERIFY(value.toObject().value(QStringLiteral("action")).toString() !=
                    QStringLiteral("bid"));
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(rows);
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(book.positions.size(), 0);
        QCOMPARE(book.exposure_usd, 0.0);
        // A refusal invents no price or size: the contract was never priced.
        QVERIFY(!untrusted.contains(QStringLiteral("price")));
        QVERIFY(!untrusted.contains(QStringLiteral("contracts")));
        QVERIFY(!untrusted.contains(QStringLiteral("quote_style")));

        // Every row — bid or pass — carries the track-record snapshot that
        // produced it, so the ledger is auditable without the report.
        const QJsonObject record = untrusted.value(QStringLiteral("track_record")).toObject();
        QCOMPARE(record.value(QStringLiteral("resolved_contracts")).toInt(), 371);
        QCOMPARE(record.value(QStringLiteral("brier_full")).toDouble(), 0.1079);
        QCOMPARE(record.value(QStringLiteral("brier_market_mid_raw")).toDouble(), 0.1083);
        QCOMPARE(record.value(QStringLiteral("adds_value_over_market")).toBool(), false);
        // Issue #171: the ledger row carries the Brier's own denominator and
        // the observation count separately, so a later audit cannot mistake
        // 12,049 correlated rows for 244 contracts of evidence. The old
        // `training_samples` reported the former under a name that read as
        // the latter, and is gone rather than silently repurposed.
        QCOMPARE(record.value(QStringLiteral("scored_contracts")).toInt(), 244);
        QCOMPARE(record.value(QStringLiteral("training_observations")).toInt(), 12'049);
        QVERIFY(!record.contains(QStringLiteral("training_samples")));
        // The handicapped baseline stays visible, distinctly named.
        QCOMPARE(record.value(QStringLiteral("brier_market_trained_logit")).toDouble(), 0.1101);
    }

    void a_report_without_a_brier_says_unavailable_rather_than_zero() {
        QJsonObject untrained = report(0.95, 0.83, 10.0, false);
        untrained.remove(QStringLiteral("brier_full"));
        untrained.insert(QStringLiteral("brier_market_mid_raw"), QJsonValue::Null);
        const QJsonArray rows = KalshiBotDecision::decide(untrained, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        const QJsonObject record = only_row(rows).value(QStringLiteral("track_record")).toObject();
        QVERIFY(record.contains(QStringLiteral("brier_available")));
        QCOMPARE(record.value(QStringLiteral("brier_available")).toBool(), false);
        QVERIFY(!record.contains(QStringLiteral("brier_full")));
        QVERIFY(!record.contains(QStringLiteral("brier_market_mid_raw")));
    }

    /// Issue #171: a schema-1 report — the one shape that carries
    /// `brier_market_baseline` and no raw mid — cannot buy trust. The
    /// calibrator rewrites the file every cycle, so this only matters across
    /// a version skew; it fails closed rather than believing a flag that was
    /// measured against a handicapped baseline.
    void a_schema_one_report_is_not_trusted() {
        QJsonObject old_schema = report(0.95, 0.35, 10.0, true);
        QVERIFY(KalshiBotDecision::signal_trusted(old_schema));          // positive control
        old_schema.remove(QStringLiteral("brier_market_mid_raw"));
        old_schema.remove(QStringLiteral("scored_contracts"));
        old_schema.insert(QStringLiteral("schema"), 1);
        old_schema.insert(QStringLiteral("training_samples"), 500);
        old_schema.insert(QStringLiteral("brier_market_baseline"), 0.1083);
        QVERIFY(!KalshiBotDecision::signal_trusted(old_schema));
        QCOMPARE(reason(KalshiBotDecision::decide(old_schema, {}, {}, kNow, {})),
                 QStringLiteral("SIGNAL_UNTRUSTED"));
    }

    /// A report claiming value over a track record it does not carry is
    /// contradicting itself, and the trust rule fails closed on it (#165) —
    /// the same reason an unknown spread is not a free one. The flag's own
    /// sample floor stays where it is measured (spot_calibrator.py); what is
    /// checked here is that the measurement is present at all.
    void a_value_claim_without_a_track_record_is_not_trusted() {
        QJsonObject claiming = report(0.95, 0.35, 10.0, true);
        QVERIFY(KalshiBotDecision::signal_trusted(claiming));           // positive control
        claiming.remove(QStringLiteral("brier_full"));
        QVERIFY(!KalshiBotDecision::signal_trusted(claiming));
        QCOMPARE(reason(KalshiBotDecision::decide(claiming, {}, {}, kNow, {})),
                 QStringLiteral("SIGNAL_UNTRUSTED"));

        QJsonObject no_baseline = report(0.95, 0.35, 10.0, true);
        no_baseline.insert(QStringLiteral("brier_market_mid_raw"), QJsonValue::Null);
        QVERIFY(!KalshiBotDecision::signal_trusted(no_baseline));
        QCOMPARE(action(KalshiBotDecision::decide(no_baseline, {}, {}, kNow, {})),
                 QStringLiteral("pass"));
    }

    // --- the resting tier's adverse-selection premium (issue #165) ---------

    /// Criteria (b) and (c): the same edge that is too thin to REST is allowed
    /// to CROSS, and one cent more of it rests. The asymmetry is the point —
    /// a resting fill is adversely selected, a crossing fill is bought.
    void a_rest_must_clear_the_premium_and_a_cross_need_not() {
        KalshiBotDecision::Config config;
        QCOMPARE(config.edge_threshold, 0.10);
        QCOMPARE(config.rest_premium_usd, 0.03);

        // |0.95 − 0.83| = 0.12: past the 0.10 base threshold, short of the
        // 0.13 the resting tier now demands. No book, so it can only rest.
        const QJsonObject thin =
            only_row(KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, config));
        QCOMPARE(thin.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
        QCOMPARE(thin.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("REST_EDGE_BELOW_PREMIUM"));
        // It is NOT the base threshold refusing this: the row's own edge is
        // above it, and the row says so.
        QVERIFY(thin.value(QStringLiteral("edge")).toDouble() >
                thin.value(QStringLiteral("edge_threshold")).toDouble());
        QVERIFY(!thin.contains(QStringLiteral("contracts")));

        // (d) The hurdle arithmetic, journaled: edge, threshold, premium, sum.
        QCOMPARE(thin.value(QStringLiteral("quote_style")).toString(), QStringLiteral("rest"));
        QCOMPARE(thin.value(QStringLiteral("side_edge")).toDouble(), 0.12);
        QCOMPARE(thin.value(QStringLiteral("edge_threshold")).toDouble(), 0.10);
        QCOMPARE(thin.value(QStringLiteral("rest_premium_usd")).toDouble(), 0.03);
        QCOMPARE(thin.value(QStringLiteral("rest_threshold")).toDouble(), 0.13);
        QCOMPARE(thin.value(QStringLiteral("rest_threshold")).toDouble(),
                 thin.value(QStringLiteral("edge_threshold")).toDouble() +
                     thin.value(QStringLiteral("rest_premium_usd")).toDouble());
        QVERIFY(thin.value(QStringLiteral("side_edge")).toDouble() <
                thin.value(QStringLiteral("rest_threshold")).toDouble());

        // (b) The SAME contract and the SAME edge, with a book tight enough to
        // clear the crossing arithmetic: it crosses. The crossing hurdle is
        // untouched by the premium, which is exactly the asymmetry #165 wants.
        const QJsonObject crossing = only_row(KalshiBotDecision::decide(
            with_book(report(0.95, 0.83, 10.0), {{QStringLiteral("market_yes_bid"), 0.82},
                                                 {QStringLiteral("market_yes_ask"), 0.84}}),
            {}, {}, kNow, config));
        QCOMPARE(crossing.value(QStringLiteral("action")).toString(), QStringLiteral("bid"));
        QCOMPARE(crossing.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(crossing.value(QStringLiteral("price")).toDouble(), 0.84);
        QCOMPARE(crossing.value(QStringLiteral("side_edge")).toDouble(),
                 thin.value(QStringLiteral("side_edge")).toDouble());
        // The premium is the resting tier's hurdle and is not claimed of a
        // cross: a crossing row that carried it would read as having cleared
        // something it was never judged against.
        QVERIFY(!crossing.contains(QStringLiteral("rest_threshold")));

        // (c) One more cent of edge — the only thing changed — and the same
        // bookless contract rests exactly as it did before this rung.
        const QJsonObject clears =
            only_row(KalshiBotDecision::decide(report(0.96, 0.83, 10.0), {}, {}, kNow, config));
        QCOMPARE(clears.value(QStringLiteral("action")).toString(), QStringLiteral("bid"));
        QCOMPARE(clears.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        QCOMPARE(clears.value(QStringLiteral("quote_style")).toString(), QStringLiteral("rest"));
        QCOMPARE(clears.value(QStringLiteral("price")).toDouble(), 0.83);
        // A resting BID shows the hurdle it cleared, not only the ones that failed.
        QCOMPARE(clears.value(QStringLiteral("rest_threshold")).toDouble(), 0.13);
        QCOMPARE(clears.value(QStringLiteral("rest_premium_usd")).toDouble(), 0.03);
        QVERIFY(clears.value(QStringLiteral("side_edge")).toDouble() >=
                clears.value(QStringLiteral("rest_threshold")).toDouble());
    }

    /// The premium is configuration, not a constant: raising it refuses a bid
    /// the default allows, and the row states the raised number it failed.
    void the_rest_premium_is_configurable_and_binds_at_the_configured_value() {
        KalshiBotDecision::Config config;
        config.rest_premium_usd = 0.20;   // 0.10 + 0.20 = 0.30 demanded
        const QJsonObject row =
            only_row(KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, config));
        QCOMPARE(row.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("REST_EDGE_BELOW_PREMIUM"));
        QCOMPARE(row.value(QStringLiteral("rest_threshold")).toDouble(), 0.30);
        // ... and the same report bids under the default, so the refusal is
        // the premium's doing and nothing else's.
        QCOMPARE(action(KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, {})),
                 QStringLiteral("bid"));
    }

    // --- idempotency across ticks ----------------------------------------

    void a_contract_already_held_is_not_bid_again() {
        const QJsonArray first = KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(action(first), QStringLiteral("bid"));
        const QJsonArray held{first.first()};
        const QJsonArray second =
            KalshiBotDecision::decide(report(0.98, 0.83, 10.0), held, {}, kNow + 60'000, {});
        QCOMPARE(action(second), QStringLiteral("pass"));
        QCOMPARE(reason(second), QStringLiteral("ALREADY_HELD"));
    }

    void a_contract_that_already_settled_is_never_bid_again() {
        // The report's runway comes from the daemon snapshot the calibrator
        // read, which can be stale under a freshly generated report — so a
        // resolved contract can still look like it has ten minutes to run.
        const QJsonArray bid = KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(action(bid), QStringLiteral("bid"));
        const QJsonArray settled = KalshiBotDecision::settle_paper(
            bid,
            QJsonArray{QJsonObject{{QStringLiteral("ticker"),
                                    only_row(bid).value(QStringLiteral("ticker")).toString()},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")}}},
            kNow);
        QCOMPARE(settled.size(), 1);
        const QJsonArray again =
            KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, settled, kNow + 60'000, {});
        QCOMPARE(action(again), QStringLiteral("pass"));
        QCOMPARE(reason(again), QStringLiteral("CONTRACT_SETTLED"));
    }

    void a_report_with_no_predictions_journals_one_row_not_silence() {
        QJsonObject empty = report(0.95, 0.83, 10.0);
        empty.insert(QStringLiteral("predictions"), QJsonObject());
        const QJsonArray rows = KalshiBotDecision::decide(empty, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(reason(rows), QStringLiteral("NO_PREDICTIONS"));
    }

    void malformed_predictions_are_skipped_never_guessed() {
        QJsonObject broken = report(0.95, 0.83, 10.0);
        broken.insert(QStringLiteral("predictions"),
                      QJsonObject{{QStringLiteral("KXBAD-1"),
                                   QJsonObject{{QStringLiteral("p_yes_full"), 0.0},
                                               {QStringLiteral("market_yes_mid"), 0.0}}}});
        const QJsonArray rows = KalshiBotDecision::decide(broken, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(reason(rows), QStringLiteral("MALFORMED_PREDICTION"));
        QVERIFY(!only_row(rows).contains(QStringLiteral("edge")));
    }

    // --- paper settlement against real results ----------------------------

    void settlements_normalize_from_both_real_ledgers_and_drop_unresolved() {
        const QJsonArray account{
            QJsonObject{{QStringLiteral("market_id"), QStringLiteral("KXA-1")},
                        {QStringLiteral("market_result"), QStringLiteral("NO")},
                        {QStringLiteral("settled_time"), QStringLiteral("2026-07-12T08:02:29Z")}},
            // No result posted yet — dropped, not guessed.
            QJsonObject{{QStringLiteral("market_id"), QStringLiteral("KXA-2")},
                        {QStringLiteral("market_result"), QString()}},
        };
        const QJsonArray labels{
            QJsonObject{{QStringLiteral("kalshi_market_id"), QStringLiteral("KXB-1")},
                        {QStringLiteral("result"), QStringLiteral("yes")},
                        {QStringLiteral("settlement_ts"), QStringLiteral("2026-07-24T14:00:00Z")}},
            QJsonObject{{QStringLiteral("kalshi_market_id"), QStringLiteral("KXB-2")},
                        {QStringLiteral("result"), QString()}},
        };
        const QJsonArray out = KalshiBotDecision::normalize_settlements(account, labels);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).toObject().value(QStringLiteral("ticker")).toString(), QStringLiteral("KXA-1"));
        QCOMPARE(out.at(0).toObject().value(QStringLiteral("market_result")).toString(), QStringLiteral("NO"));
        QCOMPARE(out.at(0).toObject().value(QStringLiteral("source")).toString(),
                 QStringLiteral("kalshi-account-settlements.jsonl"));
        QCOMPARE(out.at(1).toObject().value(QStringLiteral("ticker")).toString(), QStringLiteral("KXB-1"));
        QCOMPARE(out.at(1).toObject().value(QStringLiteral("market_result")).toString(), QStringLiteral("YES"));
    }

    void paper_positions_settle_on_the_real_result_and_otherwise_stay_open() {
        // A bid is an order (rung 6); it settles only once something filled
        // it, so the position under test comes from the replayed book.
        const QJsonArray order = KalshiBotDecision::decide(report(0.98, 0.83, 10.0), {}, {}, kNow, {});
        const QJsonArray fill = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(order), report(0.98, 0.82, 10.0, true, kNow + 1000), {},
            kNow + 1000, {}, [](const QJsonObject&, const QString&) { return true; });
        QJsonArray ledger = order;
        for (const auto& value : fill) ledger.append(value);
        const QJsonArray bid = KalshiBotOrders::replay(ledger).positions;
        QCOMPARE(bid.size(), 1);

        const QString ticker = only_row(bid).value(QStringLiteral("ticker")).toString();
        const double stake = only_row(bid).value(QStringLiteral("stake_usd")).toDouble();
        const double fee = only_row(bid).value(QStringLiteral("fee_usd")).toDouble();
        const int contracts = only_row(bid).value(QStringLiteral("contracts")).toInt();

        // No settlement record for this ticker → no settlement row at all.
        QCOMPARE(KalshiBotDecision::settle_paper(bid, {}, kNow).size(), 0);

        const QJsonArray won = KalshiBotDecision::settle_paper(
            bid,
            QJsonArray{QJsonObject{{QStringLiteral("ticker"), ticker},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")},
                                   {QStringLiteral("source"), QStringLiteral("kalshi-settlements.jsonl")}}},
            kNow);
        QCOMPARE(won.size(), 1);
        QCOMPARE(won.first().toObject().value(QStringLiteral("won")).toBool(), true);
        QCOMPARE(won.first().toObject().value(QStringLiteral("realized_pnl")).toDouble(),
                 contracts * 1.0 - stake - fee);

        const QJsonArray lost = KalshiBotDecision::settle_paper(
            bid,
            QJsonArray{QJsonObject{{QStringLiteral("ticker"), ticker},
                                   {QStringLiteral("market_result"), QStringLiteral("NO")}}},
            kNow);
        QCOMPARE(lost.first().toObject().value(QStringLiteral("won")).toBool(), false);
        QCOMPARE(lost.first().toObject().value(QStringLiteral("realized_pnl")).toDouble(), -stake - fee);
    }

    void settle_paper_copies_bid_snapshot_and_optional_mid_at_settle() {
        QJsonObject position = open_position(QStringLiteral("KXBTCD-26AUG0615-T64000"), kNow);
        position.insert(QStringLiteral("calibrated_p"), 0.71);
        position.insert(QStringLiteral("market_mid"), 0.62);
        position.insert(QStringLiteral("edge"), 0.09);
        position.insert(QStringLiteral("side_edge"), 0.09);
        position.insert(QStringLiteral("runway_seconds"), 420.0);
        position.insert(QStringLiteral("quote_style"), QStringLiteral("cross"));
        position.insert(QStringLiteral("reason_code"), QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        QHash<QString, double> mids;
        mids.insert(QStringLiteral("KXBTCD-26AUG0615-T64000"), 0.88);
        const QJsonArray settled = KalshiBotDecision::settle_paper(
            QJsonArray{position},
            QJsonArray{QJsonObject{{QStringLiteral("ticker"), QStringLiteral("KXBTCD-26AUG0615-T64000")},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")},
                                   {QStringLiteral("source"), QStringLiteral("test")}}},
            kNow, mids);
        QCOMPARE(settled.size(), 1);
        const QJsonObject row = settled.first().toObject();
        QCOMPARE(row.value(QStringLiteral("calibrated_p")).toDouble(), 0.71);
        QCOMPARE(row.value(QStringLiteral("market_mid")).toDouble(), 0.62);
        QCOMPARE(row.value(QStringLiteral("quote_style")).toString(), QStringLiteral("cross"));
        QCOMPARE(row.value(QStringLiteral("market_mid_at_settle")).toDouble(), 0.88);
        QVERIFY(!row.contains(QStringLiteral("spot_at_settle")));  // not invented
    }

    // --- paper cashout (sell-to-close) ------------------------------------

    void paper_cashout_disabled_emits_nothing() {
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        QJsonObject position = open_position(ticker, kNow);
        position.insert(QStringLiteral("side"), QStringLiteral("YES"));
        position.insert(QStringLiteral("price"), 0.70);
        KalshiBotDecision::Config config;
        config.enable_paper_cashout = false;
        KalshiBotDecision::CashoutStreak streak;
        const QJsonArray rows = KalshiBotDecision::paper_cashout(
            QJsonArray{position},
            with_book(report(0.90, 0.90, 1.0),
                      QJsonObject{{QStringLiteral("market_yes_bid"), 0.90},
                                  {QStringLiteral("market_yes_ask"), 0.92}}),
            kNow, config, &streak);
        QCOMPARE(rows.size(), 0);
    }

    void paper_cashout_refuses_when_held_side_bid_missing_even_if_mid_is_high() {
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        QJsonObject position = open_position(ticker, kNow);
        position.insert(QStringLiteral("side"), QStringLiteral("YES"));
        position.insert(QStringLiteral("price"), 0.70);
        KalshiBotDecision::Config config;
        KalshiBotDecision::CashoutStreak streak;
        // Mid is high (would look like a lock-win if mid were allowed as sell
        // proxy) but YES bid is absent — fail closed.
        const QJsonArray rows = KalshiBotDecision::paper_cashout(
            QJsonArray{position}, report(0.90, 0.95, 1.0), kNow, config, &streak);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(action(rows), QStringLiteral("pass"));
        QCOMPARE(reason(rows), QString::fromLatin1(KalshiBotDecision::kCashoutNoBid));
        QVERIFY(!only_row(rows).value(QStringLiteral("event")).toString().contains(
            QStringLiteral("settlement")));
    }

    void paper_cashout_lock_win_closes_at_floor_bid_and_replay_clears_position() {
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        QJsonObject position = open_position(ticker, kNow);
        position.insert(QStringLiteral("side"), QStringLiteral("YES"));
        position.insert(QStringLiteral("price"), 0.70);
        position.insert(QStringLiteral("contracts"), 3);
        position.insert(QStringLiteral("stake_usd"), 2.10);
        position.insert(QStringLiteral("fee_usd"), 0.05);
        KalshiBotDecision::Config config;
        KalshiBotDecision::CashoutStreak streak;
        const QJsonObject rep =
            with_book(report(0.90, 0.90, 1.0),
                      QJsonObject{{QStringLiteral("market_yes_bid"), 0.901},
                                  {QStringLiteral("market_yes_ask"), 0.92}});
        const QJsonArray rows =
            KalshiBotDecision::paper_cashout(QJsonArray{position}, rep, kNow, config, &streak);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("action")).toString(),
                 QStringLiteral("sell"));
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotDecision::kLockWin));
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("exit_price")).toDouble(), 0.90);
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("fill_model")).toString(),
                 QString::fromLatin1(KalshiBotOrders::kCashoutFillModel));

        const QJsonObject close = rows.at(1).toObject();
        QCOMPARE(close.value(QStringLiteral("event")).toString(),
                 QStringLiteral("kalshi_bot_paper_settlement"));
        QCOMPARE(close.value(QStringLiteral("resolution")).toString(),
                 QStringLiteral("early_exit"));
        QCOMPARE(close.value(QStringLiteral("close_reason")).toString(),
                 QString::fromLatin1(KalshiBotDecision::kLockWin));
        QVERIFY(close.value(QStringLiteral("won")).isNull());
        QCOMPARE(close.value(QStringLiteral("exit_price")).toDouble(), 0.90);

        // Replay: bid + fill + cashout settlement → position gone, ticker settled.
        QJsonArray ledger;
        QJsonObject bid = position;
        bid.insert(QStringLiteral("event"), QStringLiteral("kalshi_bot_decision"));
        bid.insert(QStringLiteral("order_state"), QStringLiteral("resting"));
        bid.insert(QStringLiteral("ttl_ms"), 180000.0);
        ledger.append(bid);
        ledger.append(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
            {QStringLiteral("action"), QStringLiteral("fill")},
            {QStringLiteral("position_id"), position.value(QStringLiteral("position_id"))},
            {QStringLiteral("ticker"), ticker},
            {QStringLiteral("contracts"), 3},
            {QStringLiteral("price"), 0.70},
            {QStringLiteral("fee_usd"), 0.05},
            {QStringLiteral("reason_code"), QStringLiteral("FILLED_AT_LIMIT")},
            {QStringLiteral("ts_ms"), static_cast<double>(kNow)}});
        ledger.append(rows.at(0));
        ledger.append(close);
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(ledger);
        QCOMPARE(book.positions.size(), 0);
        QVERIFY(!book.settled.isEmpty());

        // Same tick cannot re-bid: decide sees settled ticker.
        const QJsonArray decide_rows =
            KalshiBotDecision::decide(rep, {}, book.settled, kNow, config);
        bool saw_settled = false;
        for (const auto& value : decide_rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("ticker")).toString() != ticker) continue;
            QCOMPARE(row.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
            QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                     QString::fromLatin1(KalshiBotDecision::kContractSettled));
            saw_settled = true;
        }
        QVERIFY(saw_settled);
    }

    void paper_cashout_cut_requires_streak_of_two() {
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        QJsonObject position = open_position(ticker, kNow);
        position.insert(QStringLiteral("side"), QStringLiteral("YES"));
        position.insert(QStringLiteral("price"), 0.70);
        // Fair low, bid high enough that cash_out beats fair + margin.
        const QJsonObject rep =
            with_book(report(0.50, 0.55, 10.0),
                      QJsonObject{{QStringLiteral("market_yes_bid"), 0.60},
                                  {QStringLiteral("market_yes_ask"), 0.62}});
        KalshiBotDecision::Config config;
        KalshiBotDecision::CashoutStreak streak;
        const QJsonArray first =
            KalshiBotDecision::paper_cashout(QJsonArray{position}, rep, kNow, config, &streak);
        // First tick: condition true but streak=1 < 2 → quiet HOLD.
        QCOMPARE(first.size(), 0);
        QCOMPARE(streak.value(ticker), 1);

        const QJsonArray second =
            KalshiBotDecision::paper_cashout(QJsonArray{position}, rep, kNow + 1, config, &streak);
        QCOMPARE(second.size(), 2);
        QCOMPARE(second.at(0).toObject().value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotDecision::kCutEdgeReversed));
        QCOMPARE(second.at(1).toObject().value(QStringLiteral("resolution")).toString(),
                 QStringLiteral("early_exit"));
    }

    void paper_cashout_does_not_run_after_exchange_settlement_empties_positions() {
        const QString ticker = QStringLiteral("KXBTC15M-26JUL241015-15");
        QJsonObject position = open_position(ticker, kNow);
        position.insert(QStringLiteral("side"), QStringLiteral("YES"));
        const QJsonArray exchange = KalshiBotDecision::settle_paper(
            QJsonArray{position},
            QJsonArray{QJsonObject{{QStringLiteral("ticker"), ticker},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")},
                                   {QStringLiteral("source"), QStringLiteral("test")}}},
            kNow);
        QCOMPARE(exchange.size(), 1);
        QVERIFY(exchange.first().toObject().value(QStringLiteral("won")).isBool());
        // After exchange settle the caller re-replays; cashout sees empty book.
        KalshiBotDecision::Config config;
        KalshiBotDecision::CashoutStreak streak;
        const QJsonArray cashout = KalshiBotDecision::paper_cashout(
            {},
            with_book(report(0.90, 0.90, 1.0),
                      QJsonObject{{QStringLiteral("market_yes_bid"), 0.90}}),
            kNow, config, &streak);
        QCOMPARE(cashout.size(), 0);
    }

    // --- Fix 2: an aged orphan with no settlement record is voided, not held ---

    void an_aged_orphan_with_no_settlement_is_voided_and_retired() {
        const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
        const QJsonArray open{open_position(ticker, kNow)};

        // No settlement records at all, position is 49h old (> 48h horizon).
        const QJsonArray voids = KalshiBotDecision::settle_paper(open, {}, kNow + k49h);
        QCOMPARE(voids.size(), 1);
        const QJsonObject v = voids.first().toObject();
        QCOMPARE(v.value(QStringLiteral("event")).toString(), QStringLiteral("kalshi_bot_paper_void"));
        QCOMPARE(v.value(QStringLiteral("resolution")).toString(), QStringLiteral("unresolved_expired"));
        QVERIFY(v.value(QStringLiteral("won")).isNull());
        QCOMPARE(v.value(QStringLiteral("realized_pnl")).toDouble(), 0.0);

        // Replaying [bid, fill, void] retires the FILLED position: nothing left
        // open, no exposure. The bid row is the ledger shape KalshiBotOrders::replay
        // consumes (event:kalshi_bot_decision, action:bid, carrying order_state so
        // it registers as an ORDER rather than rung 1's assumed-fill shape); the
        // fill row (action:fill) advances that same order, matched by the
        // position_id the bid and fill share. Positive control first: [bid, fill]
        // alone leaves ONE open position with non-zero exposure, so the drop below
        // is the void's doing, not an empty ledger.
        const QJsonObject bid_row{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
            {QStringLiteral("action"), QStringLiteral("bid")},
            {QStringLiteral("ticker"), ticker},
            {QStringLiteral("side"), QStringLiteral("YES")},
            {QStringLiteral("contracts"), 3},
            {QStringLiteral("price"), 0.63},
            {QStringLiteral("order_state"), QStringLiteral("resting")},
            {QStringLiteral("ts_ms"), static_cast<double>(kNow)},
            {QStringLiteral("position_id"),
             ticker + QStringLiteral("@") + QString::number(kNow)}};
        const QJsonObject fill_row{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
            {QStringLiteral("action"), QStringLiteral("fill")},
            {QStringLiteral("position_id"), bid_row.value(QStringLiteral("position_id"))},
            {QStringLiteral("contracts"), 3},
            {QStringLiteral("price"), 0.63},
            {QStringLiteral("fee_usd"), 0.02}};
        const QJsonArray filled_only{bid_row, fill_row};
        const KalshiBotOrders::Book before = KalshiBotOrders::replay(filled_only);
        QCOMPARE(before.positions.size(), 1);
        QVERIFY(before.exposure_usd > 0.0);

        QJsonArray with_void = filled_only;
        with_void.append(v);
        const KalshiBotOrders::Book after = KalshiBotOrders::replay(with_void);
        QCOMPARE(after.positions.size(), 0);   // the void retired it
        QCOMPARE(after.exposure_usd, 0.0);
    }

    void a_matching_settlement_settles_and_never_voids_even_when_old() {
        const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
        const QJsonArray open{open_position(ticker, kNow)};
        const QJsonArray settlements{QJsonObject{{QStringLiteral("ticker"), ticker},
                                                 {QStringLiteral("market_result"), QStringLiteral("YES")},
                                                 {QStringLiteral("settled_time"), QStringLiteral("t")},
                                                 {QStringLiteral("source"), QStringLiteral("kalshi-settlements.jsonl")}}};
        const QJsonArray rows = KalshiBotDecision::settle_paper(open, settlements, kNow + k49h);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toObject().value(QStringLiteral("event")).toString(),
                 QStringLiteral("kalshi_bot_paper_settlement"));   // settled, not voided
    }

    void a_young_orphan_is_not_voided() {
        const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
        const QJsonArray open{open_position(ticker, kNow)};
        const QJsonArray rows = KalshiBotDecision::settle_paper(open, {}, kNow + k1h);  // 1h < 48h
        QCOMPARE(rows.size(), 0);   // stays open, no void
    }

    /// The gate and funnel count only kalshi_bot_paper_settlement
    /// (KalshiBotGate.cpp:323, KalshiBotFunnel.cpp:84), so a void is a distinct
    /// event and is never scored as a settled bid. Lock the event name in.
    void a_void_is_not_a_settlement_event() {
        const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
        const QJsonArray voids = KalshiBotDecision::settle_paper({open_position(ticker, kNow)}, {}, kNow + k49h);
        QVERIFY(voids.first().toObject().value(QStringLiteral("event")).toString() !=
                QStringLiteral("kalshi_bot_paper_settlement"));
    }

    // --- Fix 2's DISK-READ seam (whole-branch review, Important): a void must
    // retire its position DURABLY once journaled, not just in the in-memory
    // arrays the tests above hand settle_paper()/replay() directly ----------
    //
    // `run_tick()` (KalshiBotCommands.cpp) does not replay whatever array a
    // caller built in memory: every tick it re-reads the WHOLE ledger from
    // disk through `kalshi_bot_read_ledger()` with a predicate that decides
    // which event kinds survive the read. That predicate used to keep only
    // kalshi_bot_decision and kalshi_bot_paper_settlement, silently dropping
    // kalshi_bot_paper_void on the way off disk. The tests above never catch
    // that: they construct `[bid, fill, void]` as one in-memory QJsonArray and
    // hand it straight to KalshiBotOrders::replay(), so the disk-read
    // predicate is simply never in the path. This test drives the real seam —
    // a void written to an actual file, read back through the same
    // `kalshi_bot_read_ledger()` + predicate `run_tick()` uses — so a
    // regression in that predicate (this one, or a future one) fails here.
    void a_void_read_back_from_disk_retires_the_position_durably() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ledger_path = dir.filePath(QStringLiteral("kalshi-bot-decisions.jsonl"));
        const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
        const qint64 placed_ms = kNow - k49h;

        // A "rung 1" legacy bid row (no order_state key): KalshiBotOrders::replay
        // treats it as already filled at its own price/contracts/fee — the
        // ledger shape a real open position was recorded in.
        const QJsonObject bid_row = open_position(ticker, placed_ms);
        // The void a PRIOR tick already journaled for this exact orphan: the
        // real row settle_paper() emits for an aged position with no
        // settlement record, not a hand-built stand-in.
        const QJsonObject void_row =
            KalshiBotDecision::settle_paper({bid_row}, {}, kNow).first().toObject();
        QCOMPARE(void_row.value(QStringLiteral("event")).toString(),
                 QStringLiteral("kalshi_bot_paper_void"));

        QFile file(ledger_path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QJsonObject raw_bid_row{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
            {QStringLiteral("ts_ms"), static_cast<double>(placed_ms)},
            {QStringLiteral("action"), QStringLiteral("bid")},
            {QStringLiteral("ticker"), ticker},
            {QStringLiteral("side"), QStringLiteral("YES")},
            {QStringLiteral("contracts"), 3},
            {QStringLiteral("price"), 0.63},
            {QStringLiteral("fee_usd"), 0.02},
            {QStringLiteral("position_id"), bid_row.value(QStringLiteral("position_id"))}};
        for (const QJsonObject& row : {raw_bid_row, void_row})
            file.write(QJsonDocument(row).toJson(QJsonDocument::Compact) + "\n");
        file.close();

        // Read the ledger back exactly as run_tick() does: through the shared
        // predicate, off a real file — not the in-memory array the tests
        // above build by hand.
        const QJsonArray from_disk = kalshi_bot_read_ledger(ledger_path, kalshi_bot_is_replay_event);
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(from_disk);
        // The void retired the position DURABLY: nothing open, no exposure.
        QCOMPARE(book.positions.size(), 0);
        QCOMPARE(book.exposure_usd, 0.0);

        // And a second settle pass over that same re-read record must not
        // mint a second void for an orphan it already closed out — the
        // durability claim this whole test exists to prove. run_tick() only
        // calls settle_paper() when book.positions is non-empty; asserting
        // that directly is the load-bearing check, and calling settle_paper()
        // here too (on the now-empty book) confirms there is nothing left to
        // re-void even if that guard were ever removed.
        const QJsonArray revoided = KalshiBotDecision::settle_paper(book.positions, {}, kNow + 1000);
        QCOMPARE(revoided.size(), 0);
    }

    // --- KXBTC15M directional family routing --------------------------------

    void is_kxbtc15m_ticker_matches_family_prefix() {
        QVERIFY(KalshiBotDecision::is_kxbtc15m_ticker(QStringLiteral("KXBTC15M-26AUG071230-30")));
        QVERIFY(!KalshiBotDecision::is_kxbtc15m_ticker(QStringLiteral("KXBTCD-26AUG0712-T64000")));
        QVERIFY(!KalshiBotDecision::is_kxbtc15m_ticker(QStringLiteral("KXBTC-26AUG07-B65050")));
    }

    void is_commodity_15m_ticker_matches_gold_silver_wti() {
        QVERIFY(KalshiBotDecision::is_commodity_15m_ticker(QStringLiteral("KXGOLD15M-26AUG072030-30")));
        QVERIFY(KalshiBotDecision::is_commodity_15m_ticker(QStringLiteral("KXSILVER15M-26AUG072030-30")));
        QVERIFY(KalshiBotDecision::is_commodity_15m_ticker(QStringLiteral("KXWTI15M-26AUG072030-30")));
        QVERIFY(!KalshiBotDecision::is_commodity_15m_ticker(QStringLiteral("KXBTC15M-26AUG072030-30")));
        QVERIFY(!KalshiBotDecision::is_commodity_15m_ticker(QStringLiteral("KXGOLDD-26AUG07")));
    }

    void filter_commodity_15m_keeps_only_commodity_races() {
        const QJsonObject report_obj{
            {QStringLiteral("generated_at_ms"), static_cast<double>(kNow)},
            {QStringLiteral("predictions"),
             QJsonObject{{QStringLiteral("KXGOLD15M-A"), QJsonObject{{QStringLiteral("p_yes_full"), 0.6}}},
                         {QStringLiteral("KXBTC15M-A"), QJsonObject{{QStringLiteral("p_yes_full"), 0.7}}},
                         {QStringLiteral("KXBTCD-A"), QJsonObject{{QStringLiteral("p_yes_full"), 0.55}}}}}};
        const QJsonObject filtered = KalshiBotDecision::filter_commodity_15m_predictions(report_obj);
        QCOMPARE(filtered.value(QStringLiteral("predictions")).toObject().size(), 1);
        QVERIFY(filtered.value(QStringLiteral("predictions"))
                    .toObject()
                    .contains(QStringLiteral("KXGOLD15M-A")));
        const QJsonObject thr =
            KalshiBotDecision::filter_predictions_for_family(report_obj, /*keep_kxbtc15m=*/false);
        QCOMPARE(thr.value(QStringLiteral("predictions")).toObject().size(), 1);
        QVERIFY(thr.value(QStringLiteral("predictions"))
                    .toObject()
                    .contains(QStringLiteral("KXBTCD-A")));
    }

    void filter_predictions_keeps_only_the_requested_family() {
        QJsonObject mixed = report(0.95, 0.83, 10.0, true);
        QJsonObject predictions = mixed.value(QStringLiteral("predictions")).toObject();
        predictions.insert(QStringLiteral("KXBTCD-26AUG0712-T64000"), prediction(0.90, 0.70, 20.0));
        mixed.insert(QStringLiteral("predictions"), predictions);

        const QJsonObject only_15 =
            KalshiBotDecision::filter_predictions_for_family(mixed, /*keep_kxbtc15m=*/true);
        QCOMPARE(only_15.value(QStringLiteral("predictions")).toObject().size(), 1);
        QVERIFY(only_15.value(QStringLiteral("predictions"))
                    .toObject()
                    .contains(QStringLiteral("KXBTC15M-26JUL241015-15")));
        // Trust fields survive the filter so signal_trusted still answers.
        QVERIFY(KalshiBotDecision::signal_trusted(only_15));

        const QJsonObject only_threshold =
            KalshiBotDecision::filter_predictions_for_family(mixed, /*keep_kxbtc15m=*/false);
        QCOMPARE(only_threshold.value(QStringLiteral("predictions")).toObject().size(), 1);
        QVERIFY(only_threshold.value(QStringLiteral("predictions"))
                    .toObject()
                    .contains(QStringLiteral("KXBTCD-26AUG0712-T64000")));
    }

    void merge_family_reports_takes_each_family_from_its_source() {
        QJsonObject threshold = report(0.90, 0.70, 20.0, true);
        QJsonObject preds;
        preds.insert(QStringLiteral("KXBTCD-26AUG0712-T64000"), prediction(0.90, 0.70, 20.0));
        // A 15m ticker in the threshold report must NOT survive the merge —
        // the directional calibrator owns that family.
        preds.insert(QStringLiteral("KXBTC15M-SHOULD-DROP"), prediction(0.99, 0.50, 10.0));
        threshold.insert(QStringLiteral("predictions"), preds);

        QJsonObject dir15 = report(0.95, 0.83, 10.0, false);
        dir15.insert(QStringLiteral("event"), QStringLiteral("kxbtc15m_calibrator"));

        QJsonObject commod{
            {QStringLiteral("generated_at_ms"), static_cast<double>(kNow)},
            {QStringLiteral("event"), QStringLiteral("commodities_15m_calibrator")},
            {QStringLiteral("predictions"),
             QJsonObject{{QStringLiteral("KXGOLD15M-26AUG072030-30"),
                          prediction(0.61, 0.40, 10.0)}}}};

        KalshiBotDecision::Config config;
        const QJsonObject merged =
            KalshiBotDecision::merge_family_reports(threshold, dir15, kNow, config, commod);
        const QJsonObject out = merged.value(QStringLiteral("predictions")).toObject();
        QCOMPARE(out.size(), 3);
        QVERIFY(out.contains(QStringLiteral("KXBTCD-26AUG0712-T64000")));
        QVERIFY(out.contains(QStringLiteral("KXBTC15M-26JUL241015-15")));
        QVERIFY(out.contains(QStringLiteral("KXGOLD15M-26AUG072030-30")));
        QVERIFY(!out.contains(QStringLiteral("KXBTC15M-SHOULD-DROP")));
    }

    void an_untrusted_kxbtc15m_report_does_not_bid() {
        QJsonObject dir15 = report(0.98, 0.83, 10.0, /*trusted=*/false);
        dir15.insert(QStringLiteral("event"), QStringLiteral("kxbtc15m_calibrator"));
        const QJsonObject filtered =
            KalshiBotDecision::filter_predictions_for_family(dir15, /*keep_kxbtc15m=*/true);
        const QJsonArray rows = KalshiBotDecision::decide(filtered, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(reason(rows), QStringLiteral("SIGNAL_UNTRUSTED"));
        QCOMPARE(action(rows), QStringLiteral("pass"));
    }

    void a_trusted_kxbtc15m_report_with_edge_reaches_the_bid_path() {
        QJsonObject dir15 = report(0.98, 0.83, 10.0, /*trusted=*/true);
        dir15.insert(QStringLiteral("event"), QStringLiteral("kxbtc15m_calibrator"));
        const QJsonObject filtered =
            KalshiBotDecision::filter_predictions_for_family(dir15, /*keep_kxbtc15m=*/true);
        QVERIFY(KalshiBotDecision::signal_trusted(filtered));
        const QJsonArray rows = KalshiBotDecision::decide(filtered, {}, {}, kNow, {});
        QCOMPARE(action(rows), QStringLiteral("bid"));
        QCOMPARE(reason(rows), QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        QCOMPARE(only_row(rows).value(QStringLiteral("ticker")).toString(),
                 QStringLiteral("KXBTC15M-26JUL241015-15"));
    }

    void paper_bid_pause_is_quiet_when_gate_missing_or_cap_absent() {
        KalshiBotDecision::PaperGenerationRisk deep;
        deep.max_drawdown_usd = 48.33;
        QVERIFY(!KalshiBotDecision::paper_bid_pause({}, deep).paused);
        QVERIFY(!KalshiBotDecision::paper_bid_pause(
                     QJsonObject{{"evaluated", false}, {"verdict", "FAIL"}}, deep)
                     .paused);
    }

    void score_paper_generation_tracks_running_drawdown() {
        const QJsonArray rows{
            QJsonObject{{"event", "kalshi_bot_paper_settlement"},
                        {"position_id", "a"},
                        {"realized_pnl", 10.0}},
            QJsonObject{{"event", "kalshi_bot_paper_settlement"},
                        {"position_id", "b"},
                        {"realized_pnl", -25.0}},
            QJsonObject{{"event", "kalshi_bot_decision"},
                        {"position_id", "ignore"},
                        {"realized_pnl", -99.0}},
            QJsonObject{{"event", "kalshi_bot_paper_settlement"},
                        {"position_id", "a"},
                        {"realized_pnl", -25.0}},  // duplicate id ignored
        };
        const auto risk = KalshiBotDecision::score_paper_generation(rows);
        QCOMPARE(risk.settled_bids, 2);
        QCOMPARE(risk.net_pnl_usd, -15.0);
        QCOMPARE(risk.max_drawdown_usd, 25.0);
    }

    void paper_bid_pause_trips_on_current_gen_drawdown_and_requests_rotate() {
        const QJsonObject gate{
            {"evaluated", true},
            {"verdict", "FAIL"},
            {"params", QJsonObject{{"max_drawdown_usd", 20.0}}},
            {"ledger", QJsonObject{{"max_drawdown_usd", 5.0}}}};
        KalshiBotDecision::PaperGenerationRisk current;
        current.settled_bids = 12;
        current.max_drawdown_usd = 48.33;
        current.net_pnl_usd = -30.0;
        const auto pause = KalshiBotDecision::paper_bid_pause(gate, current);
        QVERIFY(pause.paused);
        QVERIFY(pause.should_rotate);
        QCOMPARE(pause.reason_code, QStringLiteral("DRAWDOWN_CAP"));
        QVERIFY(pause.detail.contains(QStringLiteral("48.33")));
        QVERIFY(pause.detail.contains(QStringLiteral("current paper generation")));
    }

    void paper_bid_pause_ignores_lifetime_gate_fail_when_current_gen_ok() {
        const QJsonObject gate{
            {"evaluated", true},
            {"verdict", "FAIL"},
            {"params", QJsonObject{{"max_drawdown_usd", 20.0}}},
            {"ledger", QJsonObject{{"max_drawdown_usd", 48.33}, {"net_pnl_usd", -37.8}}}};
        KalshiBotDecision::PaperGenerationRisk current;
        current.settled_bids = 3;
        current.max_drawdown_usd = 5.0;
        const auto pause = KalshiBotDecision::paper_bid_pause(gate, current);
        QVERIFY(!pause.paused);
        QVERIFY(!pause.should_rotate);
    }

    void paper_bid_pause_clear_when_current_gen_within_cap() {
        const QJsonObject gate{
            {"evaluated", true},
            {"verdict", "PASS"},
            {"params", QJsonObject{{"max_drawdown_usd", 20.0}}},
            {"ledger", QJsonObject{{"max_drawdown_usd", 5.0}}}};
        KalshiBotDecision::PaperGenerationRisk current;
        current.max_drawdown_usd = 5.0;
        QVERIFY(!KalshiBotDecision::paper_bid_pause(gate, current).paused);
    }

    // Regression: adds_value_over_market is measured over EVERY resolved
    // contract, a population dominated by far-from-strike contracts the bot
    // never bets. Trust must additionally require the model to beat the mid on
    // the contracts whose edge cleared the bid threshold.
    void signal_untrusted_when_it_loses_where_it_bets() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), false},
            {QStringLiteral("brier_eligible_full"), 0.2576},
            {QStringLiteral("brier_eligible_market_mid_raw"), 0.2130}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }

    void signal_trusted_when_it_wins_where_it_bets() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), true},
            {QStringLiteral("brier_eligible_full"), 0.2130},
            {QStringLiteral("brier_eligible_market_mid_raw"), 0.2576}};
        QVERIFY(KalshiBotDecision::signal_trusted(report));
    }

    // A report predating this change cannot confer trust: the same fail-closed
    // rule the existing conjuncts apply to a missing Brier.
    void signal_untrusted_when_the_eligible_fields_are_absent() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }

    // Claiming value over a track record it does not carry is self-contradiction.
    void signal_untrusted_when_eligible_flag_lacks_its_briers() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), true}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }

};

QTEST_APPLESS_MAIN(TestKalshiBotDecision)
#include "tst_kalshi_bot_decision.moc"
