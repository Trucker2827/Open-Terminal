#include "services/prediction/kalshi/KalshiBotDecision.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <cmath>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;

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
/// ≥100-sample gate) carrying one contract.
QJsonObject report(double p_full, double market_mid, double minutes_left,
                   bool trusted = true, qint64 generated_ms = kNow) {
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("event"), QStringLiteral("spot_calibrator")},
        {QStringLiteral("advisory_only"), true},
        {QStringLiteral("generated_at_ms"), static_cast<double>(generated_ms)},
        {QStringLiteral("resolved_contracts"), 371},
        {QStringLiteral("training_samples"), 500},
        {QStringLiteral("brier_full"), 0.1079},
        {QStringLiteral("brier_market_baseline"), 0.1083},
        {QStringLiteral("adds_value_over_market"), trusted},
        {QStringLiteral("predictions"),
         QJsonObject{{QStringLiteral("KXBTC15M-26JUL241015-15"),
                      prediction(p_full, market_mid, minutes_left)}}},
    };
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

} // namespace

class TestKalshiBotDecision : public QObject {
    Q_OBJECT

  private slots:
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
            KalshiBotDecision::decide(report(0.95, 0.83, 10.0, true, kNow - 119'900),
                                      {}, {}, kNow, config);
        QCOMPARE(fresh.size(), 1);
        QCOMPARE(action(fresh), QStringLiteral("bid"));
        // Exactly 120s old: refused.
        const QJsonArray stale =
            KalshiBotDecision::decide(report(0.95, 0.83, 10.0, true, kNow - 120'000),
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

        // |0.95 − 0.83| = 0.12 ≥ 0.10 → YES bid.
        const QJsonArray fat = KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, config);
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

    // --- sizing ----------------------------------------------------------

    void sizing_respects_the_stake_and_all_in_caps() {
        KalshiBotDecision::Config config;
        config.edge_threshold = 0.10;
        config.max_stake_usd = 2.00;
        config.max_all_in_usd = 3.00;
        // Limit price floors 0.835 → 0.83; 2 contracts cost $1.66, 3 would be
        // $2.49 > $2.00.
        const QJsonObject row = only_row(KalshiBotDecision::decide(report(0.95, 0.835, 10.0), {}, {}, kNow, config));
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
        const QJsonArray blocked = KalshiBotDecision::decide(report(0.95, 0.835, 10.0), {}, {}, kNow, config);
        QCOMPARE(action(blocked), QStringLiteral("pass"));
        QCOMPARE(reason(blocked), QStringLiteral("SIZE_CAP_BLOCKS_BID"));

        // One contract costing more than the whole stake cap is refused too.
        config.max_all_in_usd = 3.00;
        config.max_stake_usd = 0.50;
        const QJsonArray too_dear = KalshiBotDecision::decide(report(0.95, 0.835, 10.0), {}, {}, kNow, config);
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

    void untrusted_signal_still_papers_but_every_bid_says_so() {
        const QJsonObject untrusted =
            only_row(KalshiBotDecision::decide(report(0.95, 0.83, 10.0, false), {}, {}, kNow, {}));
        QCOMPARE(untrusted.value(QStringLiteral("action")).toString(), QStringLiteral("bid"));
        QCOMPARE(untrusted.value(QStringLiteral("reason_code")).toString(),
                 QStringLiteral("SIGNAL_UNTRUSTED"));
        QCOMPARE(untrusted.value(QStringLiteral("signal_trusted")).toBool(), false);

        // Every row — bid or pass — carries the track-record snapshot that
        // produced it, so the ledger is auditable without the report.
        const QJsonObject record = untrusted.value(QStringLiteral("track_record")).toObject();
        QCOMPARE(record.value(QStringLiteral("resolved_contracts")).toInt(), 371);
        QCOMPARE(record.value(QStringLiteral("training_samples")).toInt(), 500);
        QCOMPARE(record.value(QStringLiteral("brier_full")).toDouble(), 0.1079);
        QCOMPARE(record.value(QStringLiteral("brier_market_baseline")).toDouble(), 0.1083);
        QCOMPARE(record.value(QStringLiteral("adds_value_over_market")).toBool(), false);
    }

    void a_report_without_a_brier_says_unavailable_rather_than_zero() {
        QJsonObject untrained = report(0.95, 0.83, 10.0, false);
        untrained.remove(QStringLiteral("brier_full"));
        untrained.insert(QStringLiteral("brier_market_baseline"), QJsonValue::Null);
        const QJsonArray rows = KalshiBotDecision::decide(untrained, {}, {}, kNow, {});
        QCOMPARE(rows.size(), 1);
        const QJsonObject record = only_row(rows).value(QStringLiteral("track_record")).toObject();
        QVERIFY(record.contains(QStringLiteral("brier_available")));
        QCOMPARE(record.value(QStringLiteral("brier_available")).toBool(), false);
        QVERIFY(!record.contains(QStringLiteral("brier_full")));
        QVERIFY(!record.contains(QStringLiteral("brier_market_baseline")));
    }

    // --- idempotency across ticks ----------------------------------------

    void a_contract_already_held_is_not_bid_again() {
        const QJsonArray first = KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(action(first), QStringLiteral("bid"));
        const QJsonArray held{first.first()};
        const QJsonArray second =
            KalshiBotDecision::decide(report(0.95, 0.83, 10.0), held, {}, kNow + 60'000, {});
        QCOMPARE(action(second), QStringLiteral("pass"));
        QCOMPARE(reason(second), QStringLiteral("ALREADY_HELD"));
    }

    void a_contract_that_already_settled_is_never_bid_again() {
        // The report's runway comes from the daemon snapshot the calibrator
        // read, which can be stale under a freshly generated report — so a
        // resolved contract can still look like it has ten minutes to run.
        const QJsonArray bid = KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(action(bid), QStringLiteral("bid"));
        const QJsonArray settled = KalshiBotDecision::settle_paper(
            bid,
            QJsonArray{QJsonObject{{QStringLiteral("ticker"),
                                    only_row(bid).value(QStringLiteral("ticker")).toString()},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")}}},
            kNow);
        QCOMPARE(settled.size(), 1);
        const QJsonArray again =
            KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, settled, kNow + 60'000, {});
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
        const QJsonArray bid = KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, {});
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

    void a_settled_position_leaves_the_open_book_and_is_never_settled_twice() {
        const QJsonArray bid = KalshiBotDecision::decide(report(0.95, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(KalshiBotDecision::open_positions_from_ledger(bid, {}).size(), 1);
        const QJsonArray settled = KalshiBotDecision::settle_paper(
            bid,
            QJsonArray{QJsonObject{{QStringLiteral("ticker"),
                                    only_row(bid).value(QStringLiteral("ticker")).toString()},
                                   {QStringLiteral("market_result"), QStringLiteral("YES")}}},
            kNow);
        QCOMPARE(KalshiBotDecision::open_positions_from_ledger(bid, settled).size(), 0);
        // Passes are ledger rows too, but they are not positions.
        const QJsonArray passes = KalshiBotDecision::decide(report(0.84, 0.83, 10.0), {}, {}, kNow, {});
        QCOMPARE(action(passes), QStringLiteral("pass"));
        QCOMPARE(KalshiBotDecision::open_positions_from_ledger(passes, {}).size(), 0);
    }
};

QTEST_APPLESS_MAIN(TestKalshiBotDecision)
#include "tst_kalshi_bot_decision.moc"
