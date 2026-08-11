// Order lifecycle regressions for the Kalshi bot (ladder rung 6).
//
// The thing under test is the claim "a bid is an order, not a position": it
// rests, it costs exposure while it rests, it is pulled when its TTL runs out
// or its edge goes, and it becomes a position only when something observable
// fills it. Every case below is written against the real decide() → replay() →
// reconcile() → replay() path a tick runs, not against hand-built rows, so a
// change that makes the ledger and the book disagree fails here.

#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <cmath>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotOrders;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;
constexpr auto kTicker = "KXBTC15M-26JUL241015-15";

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

QJsonObject report(const QJsonObject& predictions, bool trusted = true, qint64 generated_ms = kNow) {
    return QJsonObject{{QStringLiteral("schema"), 2},
                       {QStringLiteral("event"), QStringLiteral("spot_calibrator")},
                       {QStringLiteral("advisory_only"), true},
                       {QStringLiteral("generated_at_ms"), static_cast<double>(generated_ms)},
                       {QStringLiteral("resolved_contracts"), 371},
                       {QStringLiteral("scored_contracts"), 244},
                       {QStringLiteral("training_observations"), 12'049},
                       {QStringLiteral("brier_full"), 0.1079},
                       {QStringLiteral("brier_market_mid_raw"), 0.1083},
                       {QStringLiteral("brier_market_trained_logit"), 0.1101},
                       {QStringLiteral("adds_value_over_market"), trusted},
                       {QStringLiteral("eligible_scored_contracts"), 200},
                       {QStringLiteral("min_eligible_contracts"), 100},
                       {QStringLiteral("families"), QJsonArray{QStringLiteral("KXBTC15M")}},
                       {QStringLiteral("adds_value_on_bet_eligible"), trusted},
                       {QStringLiteral("brier_eligible_full"), trusted ? 0.2130 : 0.2576},
                       {QStringLiteral("brier_eligible_market_mid_raw"), trusted ? 0.2576 : 0.2130},
                       {QStringLiteral("predictions"), predictions}};
}

/// One trusted contract, the same shape rung 1's tests use.
QJsonObject one(double p_full, double market_mid, double minutes_left = 10.0, bool trusted = true,
                qint64 generated_ms = kNow) {
    return report(QJsonObject{{QString::fromLatin1(kTicker),
                               prediction(p_full, market_mid, minutes_left)}},
                  trusted, generated_ms);
}

/// The bot's opening order on kTicker: YES at $0.83 × 2 (stake $1.66, fee
/// $0.02, all-in $1.68), resting.
QJsonArray opening_bid(qint64 now_ms = kNow) {
    return KalshiBotDecision::decide(one(0.98, 0.83, 10.0, true, now_ms), {}, {}, now_ms, {});
}

/// The same contract with a real book behind it (#158), which is what lets
/// decide() price the crossing tier at all.
QJsonObject one_with_book(double p_full, double market_mid, double yes_bid, double yes_ask,
                          qint64 generated_ms = kNow) {
    QJsonObject quoted = prediction(p_full, market_mid, 10.0);
    quoted.insert(QStringLiteral("market_yes_bid"), yes_bid);
    quoted.insert(QStringLiteral("market_yes_ask"), yes_ask);
    return report(QJsonObject{{QString::fromLatin1(kTicker), quoted}}, true, generated_ms);
}

/// A CROSSING order on kTicker: the $0.12 edge clears the $0.01 spread to the
/// $0.84 ask, the $0.01 taker fee there and the default $0.02 margin, so the
/// bot pays the ask instead of resting at floor(mid) = $0.83.
QJsonArray crossing_bid(qint64 now_ms = kNow) {
    return KalshiBotDecision::decide(one_with_book(0.95, 0.83, 0.82, 0.84, now_ms), {}, {}, now_ms,
                                     {});
}

QJsonObject row_at(const QJsonArray& rows, int index) { return rows.at(index).toObject(); }

QString field(const QJsonObject& row, const char* key) {
    return row.value(QLatin1String(key)).toString();
}

bool confirming_cancel(const QJsonObject&, const QString&) { return true; }
bool refusing_cancel(const QJsonObject&, const QString&) { return false; }

QJsonArray concat(const QJsonArray& a, const QJsonArray& b) {
    QJsonArray out = a;
    for (const auto& value : b) out.append(value);
    return out;
}

/// The settlement shape normalize_settlements() produces.
QJsonArray settlement(const QString& result) {
    return QJsonArray{QJsonObject{{QStringLiteral("ticker"), QString::fromLatin1(kTicker)},
                                  {QStringLiteral("market_result"), result},
                                  {QStringLiteral("source"), QStringLiteral("kalshi-settlements.jsonl")}}};
}

} // namespace

class TestKalshiBotOrders : public QObject {
    Q_OBJECT

  private slots:
    // --- a bid is an order ------------------------------------------------

    void a_bid_opens_a_resting_order_and_not_a_position() {
        const QJsonArray bid = opening_bid();
        QCOMPARE(bid.size(), 1);
        const QJsonObject row = row_at(bid, 0);
        QCOMPARE(field(row, "action"), QStringLiteral("bid"));
        QCOMPARE(field(row, "order_state"), QStringLiteral("resting"));
        QCOMPARE(row.value(QStringLiteral("filled_count")).toDouble(), 0.0);
        QCOMPARE(row.value(QStringLiteral("remaining_count")).toDouble(), 2.0);
        QCOMPARE(row.value(QStringLiteral("ttl_ms")).toDouble(), 180'000.0);
        QCOMPARE(static_cast<qint64>(row.value(QStringLiteral("expires_at_ms")).toDouble()),
                 kNow + 180'000);

        const KalshiBotOrders::Book book = KalshiBotOrders::replay(bid);
        QCOMPARE(book.resting.size(), 1);
        QCOMPARE(book.positions.size(), 0);  // nothing has filled it
        QCOMPARE(book.resting_usd, 1.66);
    }

    // --- exposure: the PR #44 rule ---------------------------------------

    void a_resting_remainder_is_exposure_at_its_limit_price() {
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(opening_bid());
        // 2 contracts × $0.83 limit, not the $0 a "no position yet" reading
        // would give, and not the all-in (fees are not exposure under #44).
        QCOMPARE(book.exposure_usd, 1.66);
        QCOMPARE(book.resting_usd, 1.66);
        QCOMPARE(book.fees_usd, 0.0);
    }

    void a_partial_fill_is_exposure_on_both_sides_of_the_split() {
        const QJsonArray bid = opening_bid();
        QJsonObject partial{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                            {QStringLiteral("ts_ms"), static_cast<double>(kNow + 1000)},
                            {QStringLiteral("ticker"), QString::fromLatin1(kTicker)},
                            {QStringLiteral("action"), QStringLiteral("fill")},
                            {QStringLiteral("reason_code"),
                             QString::fromLatin1(KalshiBotOrders::kFilledAtLimit)},
                            {QStringLiteral("position_id"),
                             row_at(bid, 0).value(QStringLiteral("position_id"))},
                            {QStringLiteral("contracts"), 1.0},
                            {QStringLiteral("price"), 0.83},
                            {QStringLiteral("fee_usd"), 0.01}};
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, {partial}));
        QCOMPARE(book.positions.size(), 1);
        QCOMPARE(row_at(book.positions, 0).value(QStringLiteral("contracts")).toDouble(), 1.0);
        QCOMPARE(row_at(book.positions, 0).value(QStringLiteral("stake_usd")).toDouble(), 0.83);
        QCOMPARE(book.resting.size(), 1);
        QCOMPARE(row_at(book.resting, 0).value(QStringLiteral("remaining_count")).toDouble(), 1.0);
        QCOMPARE(book.exposure_usd, 1.66);  // 1 filled at 0.83 + 1 resting at 0.83
    }

    void a_confirmed_cancel_releases_only_the_remainder() {
        const QJsonArray bid = opening_bid();
        const QJsonArray pulled = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(bid), one(0.99, 0.86, 10.0, true, kNow + 181'000), {},
            kNow + 181'000, {}, confirming_cancel);
        QCOMPARE(pulled.size(), 1);
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, pulled));
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(book.exposure_usd, 0.0);
    }

    // --- the exposure caps refuse bids ------------------------------------

    void a_bid_is_refused_when_resting_exposure_would_breach_the_cap() {
        // A quote resting on ANOTHER contract, at $1.66 of limit-price risk.
        KalshiBotOrders::Book book = KalshiBotOrders::replay(opening_bid());
        QCOMPARE(book.exposure_usd, 1.66);
        QJsonObject elsewhere = row_at(book.resting, 0);
        elsewhere.insert(QStringLiteral("ticker"), QStringLiteral("KXBTC15M-OTHER-15"));

        KalshiBotDecision::Exposure exposure;
        exposure.at_risk_usd = book.exposure_usd;
        exposure.resting = QJsonArray{elsewhere};

        // $1.66 resting + $1.68 all-in = $3.34, past a $3.00 ceiling.
        KalshiBotDecision::Config tight;
        tight.max_open_exposure_usd = 3.00;
        const QJsonArray refused =
            KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, tight, {}, exposure);
        QCOMPARE(refused.size(), 1);
        QCOMPARE(field(row_at(refused, 0), "action"), QStringLiteral("pass"));
        QCOMPARE(field(row_at(refused, 0), "reason_code"),
                 QStringLiteral("EXPOSURE_CAP_BLOCKS_BID"));
        QCOMPARE(row_at(refused, 0).value(QStringLiteral("exposure_used_usd")).toDouble(), 1.66);
        QCOMPARE(row_at(refused, 0).value(QStringLiteral("exposure_cap_usd")).toDouble(), 3.00);

        // The control: the SAME bid under a ceiling that fits it goes through,
        // so the refusal above cannot be blamed on anything else.
        KalshiBotDecision::Config roomy;
        roomy.max_open_exposure_usd = 5.00;
        const QJsonArray allowed =
            KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, roomy, {}, exposure);
        QCOMPARE(field(row_at(allowed, 0), "action"), QStringLiteral("bid"));
    }

    void a_bid_is_refused_when_this_run_has_spent_its_session_budget() {
        KalshiBotDecision::Exposure exposure;
        exposure.session_opened_usd = 2.00;
        KalshiBotDecision::Config config;
        config.session_budget_usd = 3.00;
        const QJsonArray refused =
            KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, config, {}, exposure);
        QCOMPARE(field(row_at(refused, 0), "reason_code"),
                 QStringLiteral("SESSION_BUDGET_BLOCKS_BID"));
        QCOMPARE(row_at(refused, 0).value(QStringLiteral("exposure_used_usd")).toDouble(), 2.00);

        config.session_budget_usd = 10.00;
        QCOMPARE(field(row_at(KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, config,
                                                        {}, exposure), 0),
                       "action"),
                 QStringLiteral("bid"));
    }

    void the_caps_bind_within_one_tick_not_only_between_ticks() {
        // Two contracts, $1.68 all-in each, under a $3.00 ceiling: the first
        // fits, the second must see the first one's exposure.
        const QJsonObject two =
            report(QJsonObject{{QStringLiteral("KXBTC15M-A"), prediction(0.98, 0.83, 10.0)},
                               {QStringLiteral("KXBTC15M-B"), prediction(0.98, 0.83, 10.0)}});
        KalshiBotDecision::Config config;
        config.max_open_exposure_usd = 3.00;
        const QJsonArray rows = KalshiBotDecision::decide(two, {}, {}, kNow, config, {}, {});
        QCOMPARE(rows.size(), 2);
        int bids = 0;
        int blocked = 0;
        for (const auto& value : rows) {
            if (field(value.toObject(), "action") == QStringLiteral("bid")) ++bids;
            if (field(value.toObject(), "reason_code") == QStringLiteral("EXPOSURE_CAP_BLOCKS_BID"))
                ++blocked;
        }
        QCOMPARE(bids, 1);
        QCOMPARE(blocked, 1);
    }

    // --- TTL, edge, settlement --------------------------------------------

    void a_ttl_expiry_cancels_the_quote_and_frees_the_ticker_to_be_requoted() {
        const QJsonArray bid = opening_bid();
        const qint64 later = kNow + 181'000;  // one second past the 180s TTL
        // The edge is still good and the market has NOT come to the limit, so
        // only the TTL can explain this cancel.
        const QJsonArray rows = KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid),
                                                           one(0.99, 0.86, 10.0, true, later), {},
                                                           later, {}, confirming_cancel);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(field(row_at(rows, 0), "action"), QStringLiteral("cancel"));
        QCOMPARE(field(row_at(rows, 0), "reason_code"), QStringLiteral("CANCELED_TTL"));
        QCOMPARE(row_at(rows, 0).value(QStringLiteral("contracts")).toDouble(), 2.0);
        QCOMPARE(row_at(rows, 0).value(QStringLiteral("released_usd")).toDouble(), 1.66);

        const QJsonObject requotable = KalshiBotOrders::requotable(rows);
        QCOMPARE(requotable.value(QString::fromLatin1(kTicker)).toString(),
                 field(row_at(bid, 0), "position_id"));

        // One second earlier the TTL has not expired and nothing is pulled.
        QCOMPARE(KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid),
                                            one(0.99, 0.86, 10.0, true, kNow + 179'000), {},
                                            kNow + 179'000, {}, confirming_cancel)
                     .size(),
                 0);
    }

    void a_collapsed_or_flipped_edge_cancels_and_is_never_requoted() {
        const QJsonArray bid = opening_bid();
        for (const QJsonObject& gone : {one(0.95, 0.90, 10.0, true, kNow + 1000),   // collapsed
                                        one(0.80, 0.90, 10.0, true, kNow + 1000)}) {  // flipped
            const QJsonArray rows = KalshiBotOrders::reconcile(
                KalshiBotOrders::replay(bid), gone, {}, kNow + 1000, {}, confirming_cancel);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(field(row_at(rows, 0), "reason_code"), QStringLiteral("CANCELED_EDGE_GONE"));
            // Cancel/replace does not replace a quote whose reason is gone.
            QVERIFY(KalshiBotOrders::requotable(rows).isEmpty());
        }
    }

    void an_edge_that_cannot_be_read_is_not_an_edge_that_is_gone() {
        const QJsonArray bid = opening_bid();
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(bid);
        // A stale report, a missing report, and a report silent about this
        // ticker all leave the order resting: only the TTL retires it.
        QCOMPARE(KalshiBotOrders::reconcile(book, one(0.80, 0.90, 10.0, true, kNow - 500'000), {},
                                            kNow + 1000, {}, confirming_cancel)
                     .size(),
                 0);
        QCOMPARE(KalshiBotOrders::reconcile(book, {}, {}, kNow + 1000, {}, confirming_cancel).size(),
                 0);
        const QJsonObject elsewhere =
            report(QJsonObject{{QStringLiteral("KXBTC15M-OTHER"), prediction(0.95, 0.83, 10.0)}});
        QCOMPARE(
            KalshiBotOrders::reconcile(book, elsewhere, {}, kNow + 1000, {}, confirming_cancel).size(),
            0);
    }

    void a_market_that_settles_under_a_resting_quote_retires_it_without_a_position() {
        const QJsonArray bid = opening_bid();
        // The mid is at $0.82, THROUGH the $0.83 limit: everything about this
        // report says "fill". The market has already resolved, though — the
        // report's snapshot went stale under a fresh generation stamp, which
        // is the same trap rung 1 closed with CONTRACT_SETTLED. A resolved
        // contract cannot fill, whatever the quote still claims.
        const QJsonArray rows =
            KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid), one(0.95, 0.82, 10.0, true, kNow + 1000),
                                       settlement(QStringLiteral("YES")), kNow + 1000, {},
                                       confirming_cancel);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(field(row_at(rows, 0), "reason_code"), QStringLiteral("CANCELED_MARKET_SETTLED"));

        const KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, rows));
        QCOMPARE(book.positions.size(), 0);
        QCOMPARE(book.resting.size(), 0);
        // A quote the market never took wins nothing and loses nothing.
        QCOMPARE(KalshiBotDecision::settle_paper(book.positions, settlement(QStringLiteral("YES")),
                                                 kNow + 2000)
                     .size(),
                 0);

        // And the contract must stay un-quotable. It has left the book
        // entirely — no position, no resting order, and (having never filled)
        // no settlement row either — so only the replayed settled set can stop
        // the next tick from bidding a market that has already resolved.
        QCOMPARE(book.settled.size(), 1);
        const QJsonArray next = KalshiBotDecision::decide(one(0.98, 0.83, 10.0, true, kNow + 2000),
                                                          book.positions, book.settled, kNow + 2000,
                                                          {}, {});
        QCOMPARE(field(row_at(next, 0), "action"), QStringLiteral("pass"));
        QCOMPARE(field(row_at(next, 0), "reason_code"), QStringLiteral("CONTRACT_SETTLED"));
    }

    void a_filled_positions_exposure_blocks_a_bid_on_another_contract() {
        // The other half of "resting+filled exposure": a position that DID
        // fill is still money at risk until it settles, and it refuses the
        // next bid just as a resting remainder does.
        const QJsonArray bid = opening_bid();
        const QJsonArray fill =
            KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid), one(0.95, 0.82, 10.0, true, kNow + 1000),
                                       {}, kNow + 1000, {}, confirming_cancel);
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, fill));
        QCOMPARE(book.resting.size(), 0);      // nothing is resting at all
        QCOMPARE(book.positions.size(), 1);
        QCOMPARE(book.exposure_usd, 1.66);     // the filled position alone

        KalshiBotDecision::Exposure exposure;
        exposure.at_risk_usd = book.exposure_usd;
        KalshiBotDecision::Config tight;
        tight.max_open_exposure_usd = 3.00;
        const QJsonArray refused = KalshiBotDecision::decide(
            report(QJsonObject{{QStringLiteral("KXBTC15M-OTHER-15"), prediction(0.98, 0.83, 10.0)}}),
            {}, {}, kNow, tight, {}, exposure);
        QCOMPARE(field(row_at(refused, 0), "reason_code"),
                 QStringLiteral("EXPOSURE_CAP_BLOCKS_BID"));

        tight.max_open_exposure_usd = 5.00;
        QCOMPARE(field(row_at(KalshiBotDecision::decide(
                                  report(QJsonObject{{QStringLiteral("KXBTC15M-OTHER-15"),
                                                      prediction(0.98, 0.83, 10.0)}}),
                                  {}, {}, kNow, tight, {}, exposure), 0),
                       "action"),
                 QStringLiteral("bid"));
    }

    // --- fills -------------------------------------------------------------

    void an_order_the_market_traded_through_fills_at_its_own_limit() {
        const QJsonArray bid = opening_bid();
        // The mid comes back to $0.82, through the $0.83 limit.
        const QJsonArray rows =
            KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid), one(0.95, 0.82, 10.0, true, kNow + 1000),
                                       {}, kNow + 1000, {}, confirming_cancel);
        QCOMPARE(rows.size(), 1);
        const QJsonObject fill = row_at(rows, 0);
        QCOMPARE(field(fill, "action"), QStringLiteral("fill"));
        QCOMPARE(field(fill, "reason_code"), QStringLiteral("FILLED_AT_LIMIT"));
        // Filled at the limit, never at the better price it could have got:
        // the improvement would be invented, the limit is what was quoted.
        QCOMPARE(fill.value(QStringLiteral("price")).toDouble(), 0.83);
        QCOMPARE(fill.value(QStringLiteral("observed_mid")).toDouble(), 0.82);
        // The inference is stated on the row, exactly as rung 1 stated its own.
        QCOMPARE(field(fill, "fill_model"), QStringLiteral("rung6_conditional_mid"));
        QVERIFY(field(fill, "fill_rule").contains(QStringLiteral("ask proxy")));

        const KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, rows));
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(book.positions.size(), 1);
        QCOMPARE(row_at(book.positions, 0).value(QStringLiteral("contracts")).toDouble(), 2.0);
        QCOMPARE(book.exposure_usd, 1.66);  // filled quantity is still at risk
    }

    void a_fill_states_the_tier_that_actually_quoted_it() {
        // The review of #163: every clause of the passive `kFillRule` is false
        // for a crossing bid (the report DID carry a book, the mid was not the
        // ask proxy, and the quote moved to the market rather than the market
        // to the quote) — and it was being stamped on crossing fills anyway.
        // Each tier now discloses its own fill, selected on `quote_style`.
        const QJsonArray crossed = crossing_bid();
        QCOMPARE(field(row_at(crossed, 0), "quote_style"), QStringLiteral("cross"));
        QCOMPARE(row_at(crossed, 0).value(QStringLiteral("price")).toDouble(), 0.84);
        const QJsonArray cross_fill = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(crossed), one(0.95, 0.83, 10.0, true, kNow + 1000), {},
            kNow + 1000, {}, confirming_cancel);
        QCOMPARE(cross_fill.size(), 1);
        const QJsonObject crossing_row = row_at(cross_fill, 0);
        QCOMPARE(field(crossing_row, "reason_code"), QStringLiteral("FILLED_AT_LIMIT"));
        // The whole sentence, compared to the constant: a one-sided substring
        // match is what let the wrong sentence ride on these rows unnoticed.
        QCOMPARE(field(crossing_row, "fill_rule"),
                 QString::fromLatin1(KalshiBotOrders::kCrossFillRule));
        QVERIFY(field(crossing_row, "fill_rule") != QString::fromLatin1(KalshiBotOrders::kFillRule));
        // The KEY the sentence was selected by, carried on the fill row beside
        // it. Without it the funnel could only bucket these fills by matching
        // the prose — and a reader of the row alone could not check the
        // selection at all.
        QCOMPARE(field(crossing_row, "quote_style"), QStringLiteral("cross"));
        // Prose only: the mechanics are the ones this rung already had. A
        // cross still fills AT its own limit, under rung 6's model.
        QCOMPARE(crossing_row.value(QStringLiteral("price")).toDouble(), 0.84);
        QCOMPARE(crossing_row.value(QStringLiteral("observed_mid")).toDouble(), 0.83);
        QCOMPARE(field(crossing_row, "fill_model"), QStringLiteral("rung6_conditional_mid"));

        // A passive quote keeps the passive sentence, which is true of it.
        const QJsonArray rested = opening_bid();
        QCOMPARE(field(row_at(rested, 0), "quote_style"), QStringLiteral("rest"));
        const QJsonArray rest_fill = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(rested), one(0.95, 0.82, 10.0, true, kNow + 1000), {},
            kNow + 1000, {}, confirming_cancel);
        QCOMPARE(rest_fill.size(), 1);
        QCOMPARE(field(row_at(rest_fill, 0), "fill_rule"),
                 QString::fromLatin1(KalshiBotOrders::kFillRule));
        QVERIFY(field(row_at(rest_fill, 0), "fill_rule") !=
                QString::fromLatin1(KalshiBotOrders::kCrossFillRule));
        QCOMPARE(field(row_at(rest_fill, 0), "quote_style"), QStringLiteral("rest"));

        // And a rung-6 row written BEFORE #158 names no tier at all: it was
        // passive, because the crossing tier did not exist to write it.
        QJsonObject pre_158 = row_at(rested, 0);
        pre_158.remove(QStringLiteral("quote_style"));
        pre_158.remove(QStringLiteral("quote_style_reason"));
        const QJsonArray legacy_fill = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(QJsonArray{pre_158}),
            one(0.95, 0.82, 10.0, true, kNow + 1000), {}, kNow + 1000, {}, confirming_cancel);
        QCOMPARE(legacy_fill.size(), 1);
        QCOMPARE(field(row_at(legacy_fill, 0), "fill_rule"),
                 QString::fromLatin1(KalshiBotOrders::kFillRule));
        // ...and states none on its fill either. The funnel counts it under
        // `unstated` rather than under `rest`: the row was passive, but it
        // never said so, and that is the difference between reading a ledger
        // and completing it.
        QVERIFY(!row_at(legacy_fill, 0).contains(QStringLiteral("quote_style")));
    }

    void a_quote_the_market_never_reached_does_not_fill() {
        const QJsonArray bid = opening_bid();
        // $0.84 mid against a $0.83 limit: nobody hit the bid.
        const QJsonArray rows =
            KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid), one(0.99, 0.84, 10.0, true, kNow + 1000),
                                       {}, kNow + 1000, {}, confirming_cancel);
        QCOMPARE(rows.size(), 0);
        QCOMPARE(KalshiBotOrders::replay(concat(bid, rows)).positions.size(), 0);
    }

    void a_filled_position_settles_once_on_the_real_result_and_leaves_the_book() {
        const QJsonArray bid = opening_bid();
        const QJsonArray fill =
            KalshiBotOrders::reconcile(KalshiBotOrders::replay(bid), one(0.95, 0.82, 10.0, true, kNow + 1000),
                                       {}, kNow + 1000, {}, confirming_cancel);
        KalshiBotOrders::Book book = KalshiBotOrders::replay(concat(bid, fill));
        const QJsonArray settled = KalshiBotDecision::settle_paper(
            book.positions, settlement(QStringLiteral("YES")), kNow + 2000);
        QCOMPARE(settled.size(), 1);
        QCOMPARE(row_at(settled, 0).value(QStringLiteral("won")).toBool(), true);
        // 2 contracts pay $2.00 against $1.66 staked and $0.02 of fee.
        QCOMPARE(row_at(settled, 0).value(QStringLiteral("realized_pnl")).toDouble(), 0.32);

        book = KalshiBotOrders::replay(concat(concat(bid, fill), settled));
        QCOMPARE(book.positions.size(), 0);
        QCOMPARE(book.exposure_usd, 0.0);
        QCOMPARE(KalshiBotDecision::settle_paper(book.positions, settlement(QStringLiteral("YES")),
                                                 kNow + 3000)
                     .size(),
                 0);
    }

    // --- cancels that are not confirmed ------------------------------------

    void an_unconfirmed_cancel_stays_at_risk_and_is_retried_next_tick() {
        const QJsonArray bid = opening_bid();
        const qint64 later = kNow + 181'000;
        const QJsonArray refused = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(bid), one(0.99, 0.86, 10.0, true, later), {}, later, {},
            refusing_cancel);
        QCOMPARE(refused.size(), 1);
        const QJsonObject row = row_at(refused, 0);
        QCOMPARE(field(row, "reason_code"), QStringLiteral("UNCONFIRMED_CANCEL"));
        QCOMPARE(field(row, "attempted_reason_code"), QStringLiteral("CANCELED_TTL"));
        // Unknown is not gone: the order keeps its state and its exposure, and
        // nothing was released.
        QCOMPARE(field(row, "order_state"), QStringLiteral("resting"));
        QCOMPARE(row.value(QStringLiteral("released_usd")).toDouble(), 0.0);
        QCOMPARE(row.value(QStringLiteral("still_at_risk_usd")).toDouble(), 1.66);

        const KalshiBotOrders::Book after = KalshiBotOrders::replay(concat(bid, refused));
        QCOMPARE(after.resting.size(), 1);
        QCOMPARE(after.exposure_usd, 1.66);

        // Next tick: the retry lands, and only then is the order gone.
        const QJsonArray retried = KalshiBotOrders::reconcile(
            after, one(0.99, 0.86, 10.0, true, later + 60'000), {}, later + 60'000, {},
            confirming_cancel);
        QCOMPARE(retried.size(), 1);
        QCOMPARE(field(row_at(retried, 0), "reason_code"), QStringLiteral("CANCELED_TTL"));
        QCOMPARE(KalshiBotOrders::replay(concat(concat(bid, refused), retried)).exposure_usd, 0.0);
    }

    // --- cancel / replace --------------------------------------------------

    void a_ticker_with_a_working_quote_is_not_quoted_again() {
        KalshiBotDecision::Exposure exposure;
        exposure.resting = KalshiBotOrders::replay(opening_bid()).resting;
        exposure.at_risk_usd = 1.66;
        const QJsonArray rows =
            KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, {}, {}, exposure);
        QCOMPARE(field(row_at(rows, 0), "action"), QStringLiteral("pass"));
        QCOMPARE(field(row_at(rows, 0), "reason_code"), QStringLiteral("QUOTE_RESTING"));
    }

    void a_bid_replacing_a_ttl_cancelled_quote_is_journaled_as_the_replace() {
        KalshiBotDecision::Exposure exposure;
        exposure.requoted = QJsonObject{{QString::fromLatin1(kTicker), QStringLiteral("old@1")}};
        const QJsonArray rows =
            KalshiBotDecision::decide(one(0.98, 0.83), {}, {}, kNow, {}, {}, exposure);
        QCOMPARE(field(row_at(rows, 0), "action"), QStringLiteral("bid"));
        QCOMPARE(field(row_at(rows, 0), "reason_code"), QStringLiteral("REQUOTED"));
        QCOMPARE(field(row_at(rows, 0), "replaces_position_id"), QStringLiteral("old@1"));

        // An untrusted signal outranks the requote, and now by refusing it
        // outright (#165): the replace half of cancel/replace is still a bid,
        // so there is no bid, no replacement quote, and nothing back in the
        // book. The cancel that freed the ticker stands; the bot simply does
        // not re-quote it.
        const QJsonArray untrusted = KalshiBotDecision::decide(
            one(0.98, 0.83, 10.0, false), {}, {}, kNow, {}, {}, exposure);
        QCOMPARE(untrusted.size(), 1);
        QCOMPARE(field(row_at(untrusted, 0), "action"), QStringLiteral("pass"));
        QCOMPARE(field(row_at(untrusted, 0), "reason_code"), QStringLiteral("SIGNAL_UNTRUSTED"));
        QVERIFY(!row_at(untrusted, 0).contains(QStringLiteral("requote")));
        QCOMPARE(KalshiBotOrders::replay(untrusted).resting.size(), 0);
        QCOMPARE(KalshiBotOrders::replay(untrusted).exposure_usd, 0.0);
    }

    // --- the ledger rung 1 already wrote ------------------------------------

    void a_rung1_bid_replays_as_the_assumed_fill_it_was() {
        // Rung 1's rows carry no order_state: they WERE positions under its
        // stated assumed-fill model, and they stay positions here — tagged
        // with the model that made them so a mixed ledger is readable.
        QJsonObject legacy = row_at(opening_bid(), 0);
        legacy.remove(QStringLiteral("order_state"));
        legacy.remove(QStringLiteral("filled_count"));
        legacy.remove(QStringLiteral("remaining_count"));
        legacy.remove(QStringLiteral("ttl_ms"));

        const KalshiBotOrders::Book book = KalshiBotOrders::replay(QJsonArray{legacy});
        QCOMPARE(book.positions.size(), 1);
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(field(row_at(book.positions, 0), "fill_model"), QStringLiteral("rung1_assumed"));
        QCOMPARE(book.exposure_usd, 1.66);
        // And it settles exactly as it did before this rung existed.
        QCOMPARE(KalshiBotDecision::settle_paper(book.positions, settlement(QStringLiteral("YES")),
                                                 kNow + 1000)
                     .size(),
                 1);
    }

    void a_pass_is_not_an_order() {
        const QJsonArray passes = KalshiBotDecision::decide(one(0.84, 0.83), {}, {}, kNow, {});
        QCOMPARE(field(row_at(passes, 0), "action"), QStringLiteral("pass"));
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(passes);
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(book.positions.size(), 0);
        QCOMPARE(book.exposure_usd, 0.0);
    }
};

QTEST_APPLESS_MAIN(TestKalshiBotOrders)
#include "tst_kalshi_bot_orders.moc"
