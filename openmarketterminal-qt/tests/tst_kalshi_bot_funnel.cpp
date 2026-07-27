// The Kalshi bot's conversion funnel and its pace to the sealed gate (#153).
//
// Every surface showed the promotion gate's NUMERATOR — `settled 4 · required
// 300` — and nothing else. On the live ledger 304 quotes produced 5 fills and 4
// settlements, and at 2.05 settlements/day the sealed 300-settlement gate is
// ~144 days out. Neither the denominator nor the pace was stated anywhere.
//
// What is pinned here, in the order the issue's criteria state it:
//
//   1. every count is a count of rows actually seen, and a missing denominator
//      makes `fill_rate` ABSENT rather than 0.0 — while a genuine zero fill
//      rate over real bids is present as 0.0 (both directions asserted);
//   2. the pace is a measurement: absent under a one-hour span, absent at a
//      zero observed rate, and absent when the sealed params cannot be read or
//      do not match their own seal — no `inf`, no `0 days`;
//   3. the paper record only: a LIVE row never increments a count, because
//      `resting_now` comes from a book replay that already skips live rows and
//      a mixed numerator/denominator would be invisible until rung 5 arms;
//   4. settlements are counted the way KalshiBotGate::evaluate() counts them
//      (deduped by position_id, unidentifiable rows not counted), because
//      `settled_remaining` subtracts from the gate's own minimum;
//   5. absence is an ABSENT KEY in the published JSON, never a zero;
//   6. the renderer is one formatter: an unavailable file prints one refusal
//      line and NO digits, and a file older than two tick intervals carries its
//      age on every line.

#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotFunnel.h"
#include "services/prediction/kalshi/KalshiBotGate.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotFunnel;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotFunnelFile;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotGate;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotOrders;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_funnel_lines;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_read_funnel_file;
using openmarketterminal::services::prediction::kalshi_ns::kKalshiBotFunnelUnstatedTier;
using openmarketterminal::services::prediction::kalshi_ns::kKalshiBotStaleMs;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;
constexpr qint64 kHour = 3'600'000LL;
constexpr qint64 kDay = 24 * kHour;

QJsonObject bid_row(qint64 ts_ms, const QString& id, const QString& ticker = QStringLiteral("KX")) {
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                       {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("action"), QStringLiteral("bid")},
                       {QStringLiteral("reason_code"), QStringLiteral("EDGE_CLEARS_THRESHOLD")},
                       {QStringLiteral("position_id"), id},
                       {QStringLiteral("ticker"), ticker},
                       {QStringLiteral("side"), QStringLiteral("YES")},
                       {QStringLiteral("price"), 0.67},
                       {QStringLiteral("contracts"), 3},
                       {QStringLiteral("ttl_ms"), 120'000},
                       {QStringLiteral("order_state"), QStringLiteral("resting")}};
}

/// A fill row as `KalshiBotOrders::reconcile()` writes one. `quote_style` is the
/// tier that priced the order and `rule` the disclosure it carries; the defaults
/// are a row that names no tier and states the passive sentence, which is every
/// fill written before #158. A null `rule` is a row that discloses nothing.
QJsonObject fill_row(qint64 ts_ms, const QString& id, const char* quote_style = nullptr,
                     const char* rule = KalshiBotOrders::kFillRule) {
    QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                    {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)},
                    {QStringLiteral("mode"), QStringLiteral("paper")},
                    {QStringLiteral("action"), QStringLiteral("fill")},
                    {QStringLiteral("reason_code"), QString::fromLatin1(KalshiBotOrders::kFilledAtLimit)},
                    {QStringLiteral("position_id"), id},
                    {QStringLiteral("ticker"), QStringLiteral("KX")},
                    {QStringLiteral("contracts"), 3},
                    {QStringLiteral("price"), 0.67},
                    {QStringLiteral("order_state"), QStringLiteral("filled")},
                    {QStringLiteral("fill_model"), QString::fromLatin1(KalshiBotOrders::kFillModel)}};
    if (rule != nullptr) row.insert(QStringLiteral("fill_rule"), QString::fromLatin1(rule));
    if (quote_style != nullptr)
        row.insert(QStringLiteral("quote_style"), QString::fromLatin1(quote_style));
    return row;
}

QJsonObject cancel_row(qint64 ts_ms, const QString& id, const char* reason) {
    const bool confirmed = QLatin1String(reason) != QLatin1String(KalshiBotOrders::kUnconfirmedCancel);
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                       {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("action"), QStringLiteral("cancel")},
                       {QStringLiteral("reason_code"), QString::fromLatin1(reason)},
                       {QStringLiteral("position_id"), id},
                       {QStringLiteral("ticker"), QStringLiteral("KX")},
                       {QStringLiteral("contracts"), 3},
                       {QStringLiteral("order_state"),
                        QString::fromLatin1(confirmed ? "cancelled" : "resting")}};
}

QJsonObject settlement_row(qint64 ts_ms, const QString& id, double pnl = 0.45) {
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
                       {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("position_id"), id},
                       {QStringLiteral("ticker"), QStringLiteral("KX")},
                       {QStringLiteral("realized_pnl"), pnl},
                       {QStringLiteral("won"), pnl > 0.0}};
}

/// A row a LIVE tick wrote: same shape, `mode: live`.
QJsonObject live(QJsonObject row) {
    row.insert(QStringLiteral("mode"), QStringLiteral("live"));
    return row;
}

/// A record of `bids` quotes over a 48 h span, of which the first `fills` fill
/// and the first `settlements` of THOSE settle; the rest expire on TTL. Shaped
/// like the operator's own ledger, which is 95% CANCELED_TTL.
QJsonArray record(int bids, int fills, int settlements, qint64 span_ms = 2 * kDay) {
    QJsonArray rows;
    const qint64 first = kNow - span_ms;
    const qint64 step = bids > 1 ? span_ms / (bids - 1) : 0;
    for (int i = 0; i < bids; ++i) {
        const QString id = QStringLiteral("p%1").arg(i);
        rows.append(bid_row(first + i * step, id));
        if (i < fills) {
            rows.append(fill_row(first + i * step + 1, id));
            if (i < settlements) rows.append(settlement_row(first + i * step + 2, id));
        } else {
            rows.append(cancel_row(first + i * step + 1, id, KalshiBotOrders::kCanceledTtl));
        }
    }
    return rows;
}

/// The real sealed params record, written through preregister() (0444) and read
/// back through load_params_file() — the same pair the gate itself uses.
QJsonValue sealed_params(const QString& dir, int min_settled = 300) {
    const QString path = QDir(dir).filePath(QString::fromLatin1(KalshiBotGate::kParamsFile));
    QString error;
    KalshiBotGate::preregister(path,
                               QJsonObject{{QStringLiteral("min_settled_bids"), min_settled},
                                           {QStringLiteral("max_drawdown_usd"), 5.0}},
                               kNow - 10 * kDay, &error);
    return KalshiBotGate::load_params_file(path);
}

/// A record holding one PASSIVE fill and one CROSSING fill — the shape a single
/// `fill_rule` string had no honest answer for (#158 round 2). `cross_first`
/// swaps which tier's pair ARRIVES first; every row keeps its own timestamp, so
/// nothing but the arrival order differs between the two.
QJsonArray mixed_tier_record(bool cross_first) {
    QJsonArray rest;
    rest.append(bid_row(kNow - kDay, QStringLiteral("rest1")));
    rest.append(fill_row(kNow - kDay + 1, QStringLiteral("rest1"), KalshiBotDecision::kQuoteRest));
    QJsonArray cross;
    cross.append(bid_row(kNow - kHour, QStringLiteral("cross1")));
    cross.append(fill_row(kNow - kHour + 1, QStringLiteral("cross1"),
                          KalshiBotDecision::kQuoteCross, KalshiBotOrders::kCrossFillRule));

    QJsonArray rows;
    for (const auto& value : cross_first ? cross : rest) rows.append(value);
    for (const auto& value : cross_first ? rest : cross) rows.append(value);
    return rows;
}

bool has_digit(const QString& text) {
    for (const QChar c : text)
        if (c.isDigit()) return true;
    return false;
}

KalshiBotFunnelFile published(const KalshiBotFunnel& funnel, qint64 ts_ms) {
    KalshiBotFunnelFile file;
    file.available = true;
    file.object = funnel.to_json(ts_ms);
    return file;
}

} // namespace

class TestKalshiBotFunnel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir dir_;

  private slots:
    // --- criterion 1: counts are counts of rows actually seen ---------------

    void counts_every_stage_of_the_funnel() {
        QJsonArray rows;
        rows.append(bid_row(kNow - kDay, QStringLiteral("a")));
        rows.append(bid_row(kNow - kDay + 10, QStringLiteral("b")));
        rows.append(bid_row(kNow - kDay + 20, QStringLiteral("c")));
        rows.append(bid_row(kNow - kDay + 30, QStringLiteral("d")));
        rows.append(bid_row(kNow - kDay + 40, QStringLiteral("e")));
        rows.append(fill_row(kNow - kDay + 50, QStringLiteral("a")));
        rows.append(settlement_row(kNow - kDay + 60, QStringLiteral("a")));
        rows.append(cancel_row(kNow - kDay + 70, QStringLiteral("b"), KalshiBotOrders::kCanceledTtl));
        rows.append(
            cancel_row(kNow - kDay + 80, QStringLiteral("c"), KalshiBotOrders::kCanceledEdgeGone));
        rows.append(
            cancel_row(kNow - kDay + 90, QStringLiteral("d"), KalshiBotOrders::kCanceledSettled));
        rows.append(cancel_row(kNow - kDay + 100, QStringLiteral("e"),
                               KalshiBotOrders::kUnconfirmedCancel));

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.bids, 5);
        QCOMPARE(funnel.fills, 1);
        QCOMPARE(funnel.settlements, 1);
        QCOMPARE(funnel.canceled_ttl, 1);
        QCOMPARE(funnel.canceled_edge_gone, 1);
        QCOMPARE(funnel.canceled_market_settled, 1);
        QCOMPARE(funnel.unconfirmed_cancels, 1);
        QCOMPARE(funnel.rows_read, 11);
        // An unconfirmed cancel leaves its order WORKING — that is the whole
        // point of the reason code, and the funnel reads it off the same book
        // replay the exposure sum uses rather than counting rows itself.
        QCOMPARE(funnel.resting_now, 1);
    }

    void fill_rate_is_absent_without_a_denominator() {
        const KalshiBotFunnel none = KalshiBotFunnel::measure(QJsonArray{}, {});
        QVERIFY(!none.fill_rate_available);
        QVERIFY(!none.to_json(kNow).contains(QStringLiteral("fill_rate")));
    }

    void a_real_zero_fill_rate_is_present_as_zero() {
        // 40 bids, none filled: 0.0 is a MEASUREMENT here, and suppressing it
        // would hide the very failure this issue is about.
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(40, 0, 0), {});
        QVERIFY(funnel.fill_rate_available);
        QCOMPARE(funnel.fill_rate, 0.0);
        QCOMPARE(funnel.to_json(kNow).value(QStringLiteral("fill_rate")).toDouble(), 0.0);
    }

    void fill_rate_is_fills_over_bids() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(304, 5, 4), {});
        QCOMPARE(funnel.bids, 304);
        QCOMPARE(funnel.fills, 5);
        QCOMPARE(funnel.settlements, 4);
        QVERIFY(funnel.fill_rate_available);
        QVERIFY(qAbs(funnel.fill_rate - 5.0 / 304.0) < 1e-12);
    }

    // --- criterion 4's counting rule: the gate's own settlement count -------

    void settlements_are_deduped_by_position_id() {
        // The gate dedups by position_id (KalshiBotGate.cpp). A raw row count
        // here would make settled_remaining subtract from a different
        // denominator than the gate scores.
        QJsonArray rows = record(2, 2, 0);
        rows.append(settlement_row(kNow - kHour, QStringLiteral("p0")));
        rows.append(settlement_row(kNow - kHour + 1, QStringLiteral("p0")));
        QCOMPARE(KalshiBotFunnel::measure(rows, {}).settlements, 1);
    }

    void an_unidentifiable_settlement_is_not_counted_and_is_reported() {
        QJsonArray rows = record(2, 2, 0);
        QJsonObject anonymous = settlement_row(kNow - kHour, QStringLiteral("p0"));
        anonymous.remove(QStringLiteral("position_id"));
        rows.append(anonymous);
        QJsonObject unpriced = settlement_row(kNow - kHour, QStringLiteral("p1"));
        unpriced.remove(QStringLiteral("realized_pnl"));
        rows.append(unpriced);

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.settlements, 0);
        QCOMPARE(funnel.settlements_unidentifiable, 2);
    }

    // --- criterion 1, honesty rule 3: the PAPER record only ------------------

    void a_live_row_never_increments_a_paper_count() {
        QJsonArray rows = record(3, 1, 1);
        rows.append(live(bid_row(kNow - kHour, QStringLiteral("live1"))));
        rows.append(live(fill_row(kNow - kHour + 1, QStringLiteral("live1"))));
        rows.append(live(settlement_row(kNow - kHour + 2, QStringLiteral("live1"))));

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.bids, 3);
        QCOMPARE(funnel.fills, 1);
        QCOMPARE(funnel.settlements, 1);
        QCOMPARE(funnel.live_rows_skipped, 3);
        // Provenance still reports everything it was handed.
        QCOMPARE(funnel.rows_read, static_cast<int>(rows.size()));
    }

    // --- criterion 1: provenance --------------------------------------------

    void provenance_states_the_span_it_measured() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(10, 2, 2, 2 * kDay), {});
        QCOMPARE(funnel.first_ts_ms, kNow - 2 * kDay);
        QVERIFY(funnel.last_ts_ms > funnel.first_ts_ms);
        QCOMPARE(funnel.span_ms, funnel.last_ts_ms - funnel.first_ts_ms);
        const QJsonObject json = funnel.to_json(kNow);
        QCOMPARE(static_cast<qint64>(json.value(QStringLiteral("span_ms")).toDouble()),
                 funnel.span_ms);
        QCOMPARE(json.value(QStringLiteral("rows_read")).toInt(), funnel.rows_read);
    }

    void settled_per_day_is_settlements_over_the_span() {
        // 4 settlements over ~2 days ≈ 2/day — the operator's own shape.
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), {});
        QVERIFY(funnel.settled_per_day_available);
        const double expected = 4.0 * 86'400'000.0 / static_cast<double>(funnel.span_ms);
        QVERIFY(qAbs(funnel.settled_per_day - expected) < 1e-9);
        QVERIFY(funnel.settled_per_day > 1.9 && funnel.settled_per_day < 2.1);
    }

    void settled_per_day_is_absent_over_no_span() {
        QJsonArray rows{bid_row(kNow, QStringLiteral("only"))};
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.span_ms, 0);
        QVERIFY(!funnel.settled_per_day_available);
        QVERIFY(!funnel.to_json(kNow).contains(QStringLiteral("settled_per_day")));
    }

    // --- criterion 2: a pace that is a measurement, not a promise ------------

    void the_pace_to_the_sealed_gate_is_reported() {
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), sealed_params(dir_.path()));
        QVERIFY(funnel.gate_required_available);
        QCOMPARE(funnel.gate_required, 300);
        QVERIFY(funnel.pace_available);
        QCOMPARE(funnel.settled_remaining, 296);
        // 296 remaining at ~2/day is ~148 days. The arithmetic is pinned to the
        // measured rate, not to a constant.
        QVERIFY(qAbs(funnel.days_to_gate_at_observed_rate -
                     funnel.settled_remaining / funnel.settled_per_day) < 1e-9);
        QVERIFY(funnel.days_to_gate_at_observed_rate > 140.0);
        QVERIFY(funnel.days_to_gate_at_observed_rate < 160.0);
    }

    void the_pace_is_absent_under_a_one_hour_span() {
        // Ten minutes of record cannot support a projection out to days.
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(20, 4, 3, 10 * 60'000LL), sealed_params(dir_.path()));
        QVERIFY(funnel.settled_per_day_available);  // the rate itself is measurable
        QVERIFY(!funnel.pace_available);
        QVERIFY(funnel.pace_unavailable_reason.contains(QStringLiteral("hour")));
        const QJsonObject json = funnel.to_json(kNow);
        QVERIFY(!json.contains(QStringLiteral("days_to_gate_at_observed_rate")));
        QVERIFY(!json.contains(QStringLiteral("settled_remaining")));
        QVERIFY(json.contains(QStringLiteral("pace_unavailable_reason")));
    }

    void the_pace_is_absent_at_a_zero_observed_rate() {
        // 200 bids, nothing ever settled: the gate is not "0 days away" and it
        // is not `inf` days away — it is not reachable at a measured rate of 0.
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(200, 0, 0, 2 * kDay), sealed_params(dir_.path()));
        QCOMPARE(funnel.settled_per_day, 0.0);
        QVERIFY(!funnel.pace_available);
        QVERIFY(funnel.pace_unavailable_reason.contains(QStringLiteral("zero")));
        QVERIFY(!funnel.to_json(kNow).contains(QStringLiteral("days_to_gate_at_observed_rate")));
    }

    void no_params_file_means_no_target_and_no_pace() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(
            record(304, 5, 4, 2 * kDay), KalshiBotGate::load_params_file(
                                             QDir(dir_.path()).filePath(QStringLiteral("nope.json"))));
        QVERIFY(!funnel.gate_required_available);
        QVERIFY(!funnel.pace_available);
        QVERIFY(!funnel.to_json(kNow).contains(QStringLiteral("gate_required")));
    }

    void tampered_params_are_never_paced_against() {
        // The gate refuses a broken seal and publishes NO numbers. A funnel
        // that quoted the minimum out of it anyway would be pacing the operator
        // toward a target the gate itself will not honour.
        QJsonObject record_object = sealed_params(dir_.path()).toObject();
        QJsonObject params = record_object.value(QStringLiteral("params")).toObject();
        params.insert(QStringLiteral("min_settled_bids"), 4);
        record_object.insert(QStringLiteral("params"), params);
        QVERIFY(!KalshiBotGate::seal_valid(record_object));

        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), record_object);
        QVERIFY(!funnel.gate_required_available);
        QVERIFY(!funnel.pace_available);
        QVERIFY(funnel.pace_unavailable_reason.contains(QStringLiteral("seal")));
    }

    // --- criterion 4: the fill model comes off the ledger's own rows ---------

    void the_fill_model_is_the_ledgers_own_value() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(4, 2, 0), {});
        QCOMPARE(funnel.fill_models,
                 QStringList{QString::fromLatin1(KalshiBotOrders::kFillModel)});
        // `record()`'s fills predate #158 and journal no tier, so the rule they
        // do carry is stated under the tier they DON'T: `unstated`, never
        // `rest`. (The keying itself is pinned below.)
        QCOMPARE(funnel.fill_rules.size(), 1);
        QCOMPARE(funnel.fill_rules.at(0).quote_style,
                 QString::fromLatin1(kKalshiBotFunnelUnstatedTier));
        QCOMPARE(funnel.fill_rules.at(0).rule, QString::fromLatin1(KalshiBotOrders::kFillRule));
        QCOMPARE(funnel.fill_rules.at(0).fills, 2);
    }

    // --- #158: two quoting tiers, two disclosures, one record ----------------

    void a_mixed_tier_record_publishes_one_rule_per_tier() {
        // The defect: `fill_rule` was a scalar written by every fill row in
        // turn, so a record holding a passive fill AND a crossing fill
        // published whichever came last — and the passive sentence ("its
        // market mid is the ask proxy … it selects on the market having moved
        // to the quote") is false of every crossing fill.
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(mixed_tier_record(false), {});
        QCOMPARE(funnel.fills, 2);
        QCOMPARE(funnel.fill_rules.size(), 2);

        // Keyed by the tier the deciding tick journaled, in key order — the
        // ledger's order is not an input to this.
        QCOMPARE(funnel.fill_rules.at(0).quote_style,
                 QString::fromLatin1(KalshiBotDecision::kQuoteCross));
        QCOMPARE(funnel.fill_rules.at(0).rule,
                 QString::fromLatin1(KalshiBotOrders::kCrossFillRule));
        QCOMPARE(funnel.fill_rules.at(0).fills, 1);
        QCOMPARE(funnel.fill_rules.at(1).quote_style,
                 QString::fromLatin1(KalshiBotDecision::kQuoteRest));
        QCOMPARE(funnel.fill_rules.at(1).rule, QString::fromLatin1(KalshiBotOrders::kFillRule));
        QCOMPARE(funnel.fill_rules.at(1).fills, 1);

        // The published file carries both, each under its own tier.
        const QJsonObject published_rules =
            funnel.to_json(kNow).value(QStringLiteral("fill_rules")).toObject();
        QCOMPARE(published_rules.keys(),
                 QStringList({QString::fromLatin1(KalshiBotDecision::kQuoteCross),
                              QString::fromLatin1(KalshiBotDecision::kQuoteRest)}));
        QCOMPARE(published_rules.value(QString::fromLatin1(KalshiBotDecision::kQuoteCross))
                     .toObject()
                     .value(QStringLiteral("rule"))
                     .toString(),
                 QString::fromLatin1(KalshiBotOrders::kCrossFillRule));
        QCOMPARE(published_rules.value(QString::fromLatin1(KalshiBotDecision::kQuoteRest))
                     .toObject()
                     .value(QStringLiteral("rule"))
                     .toString(),
                 QString::fromLatin1(KalshiBotOrders::kFillRule));
        // No scalar survives beside them: one string over a mixed record is
        // the bug, and a reader must not be able to find one.
        QVERIFY(!funnel.to_json(kNow).contains(QStringLiteral("fill_rule")));

        // And the operator reads BOTH sentences, each next to its tier.
        const QString line = kalshi_bot_funnel_lines(published(funnel, kNow), kNow).last();
        QVERIFY(line.contains(QString::fromLatin1(KalshiBotOrders::kCrossFillRule)));
        QVERIFY(line.contains(QString::fromLatin1(KalshiBotOrders::kFillRule)));
        QVERIFY(line.contains(QStringLiteral("cross (1 fill)")));
        QVERIFY(line.contains(QStringLiteral("rest (1 fill)")));
    }

    void which_rule_is_published_never_depends_on_row_order() {
        // The reviewer's own measurement: the same two fills, the crossing pair
        // first instead of second, used to swap the published sentence.
        const KalshiBotFunnel first = KalshiBotFunnel::measure(mixed_tier_record(false), {});
        const KalshiBotFunnel second = KalshiBotFunnel::measure(mixed_tier_record(true), {});
        QCOMPARE(second.fills, first.fills);
        QCOMPARE(second.to_json(kNow), first.to_json(kNow));
        QCOMPARE(kalshi_bot_funnel_lines(published(second, kNow), kNow),
                 kalshi_bot_funnel_lines(published(first, kNow), kNow));
    }

    void every_fill_is_counted_under_exactly_one_tier() {
        // A per-tier count that only counted the rows carrying a rule would
        // under-report the record — the same defect class one level down. A
        // fill that discloses nothing is still a fill, and is still SOMEWHERE.
        QJsonArray rows = mixed_tier_record(false);
        rows.append(bid_row(kNow - kHour + 10, QStringLiteral("cross2")));
        rows.append(fill_row(kNow - kHour + 11, QStringLiteral("cross2"),
                             KalshiBotDecision::kQuoteCross, KalshiBotOrders::kCrossFillRule));
        rows.append(bid_row(kNow - kHour + 20, QStringLiteral("mute")));
        rows.append(fill_row(kNow - kHour + 21, QStringLiteral("mute"), nullptr, nullptr));

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.fills, 4);
        int counted = 0;
        for (const auto& tier : funnel.fill_rules) counted += tier.fills;
        QCOMPARE(counted, funnel.fills);
        QCOMPARE(funnel.fill_rules.size(), 3);
        QCOMPARE(funnel.fill_rules.at(0).fills, 2);   // cross
        QCOMPARE(funnel.fill_rules.at(1).fills, 1);   // rest
        // The silent one: counted, and quoted no sentence it did not carry.
        QCOMPARE(funnel.fill_rules.at(2).quote_style,
                 QString::fromLatin1(kKalshiBotFunnelUnstatedTier));
        QCOMPARE(funnel.fill_rules.at(2).fills, 1);
        QVERIFY(funnel.fill_rules.at(2).rule.isEmpty());
        QVERIFY(!funnel.to_json(kNow)
                     .value(QStringLiteral("fill_rules"))
                     .toObject()
                     .value(QString::fromLatin1(kKalshiBotFunnelUnstatedTier))
                     .toObject()
                     .contains(QStringLiteral("rule")));
        QVERIFY(kalshi_bot_funnel_lines(published(funnel, kNow), kNow)
                    .last()
                    .contains(QStringLiteral("state no fill_rule of their own")));
    }

    void a_tier_whose_rows_disagree_quotes_neither_sentence() {
        // Nothing this codebase writes can do this — the sentence is a function
        // of the tier — so it means a hand-edited or a future record. Picking
        // one of two contradicting disclosures is exactly the last-writer bug,
        // so the tier states that they disagree and quotes no sentence at all.
        QJsonArray rows;
        rows.append(bid_row(kNow - kDay, QStringLiteral("a")));
        rows.append(fill_row(kNow - kDay + 1, QStringLiteral("a"), KalshiBotDecision::kQuoteRest));
        rows.append(bid_row(kNow - kHour, QStringLiteral("b")));
        rows.append(fill_row(kNow - kHour + 1, QStringLiteral("b"), KalshiBotDecision::kQuoteRest,
                             "paper: a different sentence entirely"));

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.fill_rules.size(), 1);
        QCOMPARE(funnel.fill_rules.at(0).fills, 2);
        QVERIFY(funnel.fill_rules.at(0).rules_disagree);
        QVERIFY(funnel.fill_rules.at(0).rule.isEmpty());
        const QJsonObject entry = funnel.to_json(kNow)
                                      .value(QStringLiteral("fill_rules"))
                                      .toObject()
                                      .value(QString::fromLatin1(KalshiBotDecision::kQuoteRest))
                                      .toObject();
        QVERIFY(!entry.contains(QStringLiteral("rule")));
        QVERIFY(entry.value(QStringLiteral("rules_disagree")).toBool());
        const QString line = kalshi_bot_funnel_lines(published(funnel, kNow), kNow).last();
        QVERIFY(line.contains(QStringLiteral("DIFFERENT fill rules")));
        QVERIFY(!line.contains(QString::fromLatin1(KalshiBotOrders::kFillRule)));
    }

    void a_fill_that_states_no_tier_is_never_relabelled_as_one() {
        // Every fill on the operator's record predates #158. They were in fact
        // passive — but the ROW does not say so, and the funnel must not fill
        // that in. The sentence they carry is still printed; the tier is not
        // invented.
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(4, 2, 0), {});
        const QJsonObject rules =
            funnel.to_json(kNow).value(QStringLiteral("fill_rules")).toObject();
        QCOMPARE(rules.keys(), QStringList{QString::fromLatin1(kKalshiBotFunnelUnstatedTier)});
        QVERIFY(!rules.contains(QString::fromLatin1(KalshiBotDecision::kQuoteRest)));
        QVERIFY(!rules.contains(QString::fromLatin1(KalshiBotDecision::kQuoteCross)));
        const QString line = kalshi_bot_funnel_lines(published(funnel, kNow), kNow).last();
        QVERIFY(line.contains(QStringLiteral("unstated (2 fills)")));
        QVERIFY(line.contains(QString::fromLatin1(KalshiBotOrders::kFillRule)));
        QVERIFY(!line.contains(QString::fromLatin1(KalshiBotOrders::kCrossFillRule)));
    }

    void a_rung1_bid_is_an_assumed_fill_and_says_so() {
        // Found on the operator's own record: 9 of 304 bids predate the order
        // lifecycle (no `order_state`), and KalshiBotOrders::replay() books
        // them as ASSUMED fills at the quoted price. They state no fill_model,
        // so a funnel that only listed the models rows carry would describe a
        // record holding 9 assumed fills as purely rung-6.
        QJsonArray rows = record(4, 2, 0);
        QJsonObject legacy = bid_row(kNow - kHour, QStringLiteral("rung1"));
        legacy.remove(QStringLiteral("order_state"));
        legacy.remove(QStringLiteral("ttl_ms"));
        rows.append(legacy);

        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(rows, {});
        QCOMPARE(funnel.legacy_assumed_fill_bids, 1);
        // It is a BID, and it is NOT a fill: nothing observed it fill.
        QCOMPARE(funnel.bids, 5);
        QCOMPARE(funnel.fills, 2);
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, kNow), kNow);
        QVERIFY(lines.last().contains(QStringLiteral("1 bids predate the order lifecycle")));
        QVERIFY(lines.last().contains(QStringLiteral("ASSUMED fills")));
    }

    void a_record_with_no_legacy_bids_makes_no_assumed_fill_claim() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(4, 2, 0), {});
        QCOMPARE(funnel.legacy_assumed_fill_bids, 0);
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, kNow), kNow);
        QVERIFY(!lines.last().contains(QStringLiteral("ASSUMED")));
    }

    void a_record_that_states_no_fill_model_claims_none() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(4, 0, 0), {});
        QVERIFY(funnel.fill_models.isEmpty());
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, kNow), kNow);
        QVERIFY(lines.last().contains(QStringLiteral("FILL MODEL · not stated")));
    }

    // --- criterion 3/4: reading the published file --------------------------

    void an_absent_file_is_unavailable_with_a_reason() {
        const KalshiBotFunnelFile file =
            kalshi_bot_read_funnel_file(QDir(dir_.path()).filePath(QStringLiteral("absent.json")));
        QVERIFY(!file.available);
        QVERIFY(file.why.contains(QStringLiteral("kalshi-bot-funnel.json")));
    }

    void an_unparseable_file_is_unavailable_and_yields_no_numbers() {
        const QString path = QDir(dir_.path()).filePath(QStringLiteral("broken.json"));
        QFile handle(path);
        QVERIFY(handle.open(QIODevice::WriteOnly));
        handle.write("{\"bids\": 304, this is not json");
        handle.close();

        const KalshiBotFunnelFile file = kalshi_bot_read_funnel_file(path);
        QVERIFY(!file.available);
        QVERIFY(file.why.contains(QStringLiteral("parseable")));
        const QStringList lines = kalshi_bot_funnel_lines(file, kNow);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.first().startsWith(QStringLiteral("FUNNEL UNAVAILABLE")));
        // The bytes on disk contain "304"; the rendered refusal must not.
        QVERIFY(!lines.first().contains(QStringLiteral("304")));
    }

    void a_round_trip_through_disk_renders_the_same_lines() {
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), sealed_params(dir_.path()));
        const QString path = QDir(dir_.path()).filePath(QStringLiteral("funnel.json"));
        QFile handle(path);
        QVERIFY(handle.open(QIODevice::WriteOnly));
        handle.write(QJsonDocument(funnel.to_json(kNow)).toJson());
        handle.close();

        const KalshiBotFunnelFile file = kalshi_bot_read_funnel_file(path);
        QVERIFY(file.available);
        QCOMPARE(kalshi_bot_funnel_lines(file, kNow),
                 kalshi_bot_funnel_lines(published(funnel, kNow), kNow));
    }

    // --- criterion 4/5: the one formatter -----------------------------------

    void the_rendered_funnel_states_its_denominator() {
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), sealed_params(dir_.path()));
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, kNow), kNow);
        QCOMPARE(lines.size(), 5);
        QVERIFY(lines.at(0).contains(QStringLiteral("304 bids → 5 fills → 4 settled")));
        // The rate is never printed without the count it came from.
        QVERIFY(lines.at(1).contains(QStringLiteral("1.6%")));
        QVERIFY(lines.at(1).contains(QStringLiteral("5 of 304 bids")));
        QVERIFY(lines.at(2).contains(QStringLiteral("per day")));
        QVERIFY(lines.at(2).contains(QStringLiteral("48.0h")));
        QVERIFY(lines.at(3).contains(QStringLiteral("296 more settled bids needed of 300")));
        QVERIFY(lines.at(4).contains(QString::fromLatin1(KalshiBotOrders::kFillModel)));
    }

    void an_unavailable_funnel_renders_one_line_and_no_numbers() {
        const QStringList lines = kalshi_bot_funnel_lines(KalshiBotFunnelFile{}, kNow);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.first().startsWith(QStringLiteral("FUNNEL UNAVAILABLE")));
    }

    void a_fresh_file_carries_no_age() {
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(record(10, 2, 2), {});
        const QStringList lines =
            kalshi_bot_funnel_lines(published(funnel, kNow), kNow + kKalshiBotStaleMs);
        for (const QString& line : lines) QVERIFY(!line.contains(QStringLiteral("as of")));
    }

    void a_stale_file_carries_its_age_on_every_line() {
        // Two tick intervals is the same threshold the status chip goes stale
        // on — a funnel is a snapshot of a record that keeps growing, and a
        // stale snapshot shown as current is this issue's own failure mode.
        const KalshiBotFunnel funnel =
            KalshiBotFunnel::measure(record(304, 5, 4, 2 * kDay), sealed_params(dir_.path()));
        const qint64 published_ms = kNow - 10 * 60'000LL;
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, published_ms), kNow);
        QCOMPARE(lines.size(), 5);
        for (const QString& line : lines) QVERIFY(line.contains(QStringLiteral("· as of 10m ago")));
    }

    void every_absent_number_is_an_absent_key_never_a_zero() {
        // A record with no bid, no settlement and no span: three numbers the
        // record cannot support, none of them published as 0.
        const KalshiBotFunnel funnel = KalshiBotFunnel::measure(QJsonArray{}, {});
        const QJsonObject json = funnel.to_json(kNow);
        QVERIFY(!json.contains(QStringLiteral("fill_rate")));
        QVERIFY(!json.contains(QStringLiteral("settled_per_day")));
        QVERIFY(!json.contains(QStringLiteral("days_to_gate_at_observed_rate")));
        QVERIFY(!json.contains(QStringLiteral("settled_remaining")));
        QVERIFY(!json.contains(QStringLiteral("gate_required")));
        // The counts it CAN support are still counts.
        QCOMPARE(json.value(QStringLiteral("bids")).toInt(), 0);
        QCOMPARE(json.value(QStringLiteral("rows_read")).toInt(), 0);
        // …and the refusal lines carry no digits from a record that has none.
        const QStringList lines = kalshi_bot_funnel_lines(published(funnel, kNow), kNow);
        QVERIFY(lines.at(1).startsWith(QStringLiteral("FILL RATE UNAVAILABLE")));
        QVERIFY(!has_digit(lines.at(1)));
        QVERIFY(lines.at(3).startsWith(QStringLiteral("PACE UNAVAILABLE")));
    }
};

QTEST_MAIN(TestKalshiBotFunnel)
#include "tst_kalshi_bot_funnel.moc"
