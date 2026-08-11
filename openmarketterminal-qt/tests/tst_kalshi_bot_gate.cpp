// Sealed promotion gate for the paper Kalshi bot (ladder rung 2, issue #127).
//
// Every criterion gets a test that flips ONLY that criterion, starting from a
// baseline ledger that passes all four — so a green suite proves each check is
// bound to its own criterion rather than passing incidentally.
//
// The seal is tested through the disk, not in memory: a seal that verifies
// against an in-process record but not against the file it was written to
// would make every freshly preregistered gate read TAMPERED.

#include "services/prediction/kalshi/KalshiBotGate.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotGate;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;

/// The ladder's own floor criteria: 300 settled bids, profit after fees, a
/// Brier that beats the market, and a $50 drawdown limit.
QJsonObject baseline_params() {
    return QJsonObject{{QStringLiteral("min_settled_bids"), 300},
                       {QStringLiteral("max_drawdown_usd"), 50.0}};
}

QJsonObject sealed_record(const QJsonObject& params = baseline_params()) {
    QJsonObject record{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QStringLiteral("kalshi_bot_gate_params")},
                       {QStringLiteral("gate_id"), QStringLiteral("test-gate")},
                       {QStringLiteral("sealed_at_ms"), static_cast<double>(kNow - 86'400'000)},
                       {QStringLiteral("params"), params}};
    record.insert(QStringLiteral("seal_sha256"), KalshiBotGate::seal(record));
    return record;
}

struct Ledger {
    QJsonArray decisions;
    QJsonArray settlements;
};

/// One settled paper bid, in rung 1's ledger shape: the decision row that
/// opened it (carrying the calibrated probability and the market mid it was
/// measured against) and the settlement row that closed it.
void append_settled_bid(Ledger& ledger, int index, double calibrated_p, double market_mid,
                        const QString& result, double realized_pnl) {
    const QString ticker = QStringLiteral("KXBTC15M-T%1").arg(index);
    const QString position_id = ticker + QStringLiteral("@") + QString::number(kNow + index);
    ledger.decisions.append(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
        {QStringLiteral("ts_ms"), static_cast<double>(kNow + index)},
        {QStringLiteral("mode"), QStringLiteral("paper")},
        {QStringLiteral("ticker"), ticker},
        {QStringLiteral("action"), QStringLiteral("bid")},
        {QStringLiteral("reason_code"), QStringLiteral("EDGE_CLEARS_THRESHOLD")},
        {QStringLiteral("calibrated_p"), calibrated_p},
        {QStringLiteral("market_mid"), market_mid},
        {QStringLiteral("side"), QStringLiteral("YES")},
        {QStringLiteral("price"), 0.50},
        {QStringLiteral("contracts"), 4},
        {QStringLiteral("stake_usd"), 2.00},
        {QStringLiteral("fee_usd"), 0.07},
        {QStringLiteral("position_id"), position_id}});
    ledger.settlements.append(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
        {QStringLiteral("ts_ms"), static_cast<double>(kNow + 1'000 + index)},
        {QStringLiteral("mode"), QStringLiteral("paper")},
        {QStringLiteral("position_id"), position_id},
        {QStringLiteral("ticker"), ticker},
        {QStringLiteral("side"), QStringLiteral("YES")},
        {QStringLiteral("contracts"), 4},
        {QStringLiteral("stake_usd"), 2.00},
        {QStringLiteral("fee_usd"), 0.07},
        {QStringLiteral("market_result"), result},
        {QStringLiteral("won"), realized_pnl > 0.0},
        {QStringLiteral("realized_pnl"), realized_pnl}});
}

/// 300 settled bids, each forecast 0.75 against a 0.50 market and settled YES
/// for +$0.10: the count clears, P&L is positive, the bot's Brier (0.0625)
/// beats the market's (0.25), and the equity curve never dips. All four
/// criteria are met — every failure test below flips exactly one of them.
Ledger baseline_ledger(int count = 300, double calibrated_p = 0.75, double market_mid = 0.50,
                       double pnl = 0.10) {
    Ledger ledger;
    for (int i = 0; i < count; ++i)
        append_settled_bid(ledger, i, calibrated_p, market_mid, QStringLiteral("YES"), pnl);
    return ledger;
}

QJsonObject verdict_of(const Ledger& ledger, const QJsonValue& params = sealed_record(),
                       const KalshiBotGate::RecordIntegrity& integrity =
                           KalshiBotGate::RecordIntegrity::whole()) {
    return KalshiBotGate::evaluate(params, ledger.decisions, ledger.settlements, kNow, integrity);
}

QString verdict(const QJsonObject& out) {
    return out.value(QStringLiteral("verdict")).toString();
}

QJsonObject criterion(const QJsonObject& out, const char* id) {
    for (const auto& value : out.value(QStringLiteral("criteria")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == QLatin1String(id))
            return value.toObject();
    return {};
}

bool met(const QJsonObject& out, const char* id) {
    return criterion(out, id).value(QStringLiteral("met")).toBool();
}

/// Every criterion except `except` is met — the flip test's whole point.
void assert_only_failure(const QJsonObject& out, const char* except) {
    for (const char* id : {KalshiBotGate::kCriterionSettled, KalshiBotGate::kCriterionNetPnl,
                           KalshiBotGate::kCriterionBrier, KalshiBotGate::kCriterionDrawdown}) {
        const bool expected = qstrcmp(id, except) != 0;
        QVERIFY2(met(out, id) == expected,
                 qPrintable(QStringLiteral("criterion %1: met=%2, expected %3")
                                .arg(QLatin1String(id))
                                .arg(met(out, id))
                                .arg(expected)));
    }
}

} // namespace

class TestKalshiBotGate : public QObject {
    Q_OBJECT

  private slots:
    // --- the seal, through the disk ---------------------------------------

    /// Preregister → write → read back → verify. An in-memory-only seal test
    /// would pass even if the written file could never be verified again.
    void seal_survives_the_disk_round_trip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-gate-params.json"));

        QString error = QStringLiteral("untouched");
        const QJsonObject written =
            KalshiBotGate::preregister(path, baseline_params(), kNow, &error);
        QVERIFY2(!written.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());

        const QJsonValue reloaded = KalshiBotGate::load_params_file(path);
        QVERIFY(reloaded.isObject());
        QVERIFY2(KalshiBotGate::seal_valid(reloaded.toObject()),
                 "a freshly preregistered file must verify against its own seal after a "
                 "write/parse round trip");
        QCOMPARE(reloaded.toObject().value(QStringLiteral("seal_sha256")).toString(),
                 written.value(QStringLiteral("seal_sha256")).toString());
        // And it evaluates: the round trip does not silently produce TAMPERED.
        QCOMPARE(verdict(verdict_of(baseline_ledger(), reloaded)),
                 QString::fromLatin1(KalshiBotGate::kVerdictPass));
    }

    void preregistered_params_are_written_read_only() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-gate-params.json"));
        QString error;
        QVERIFY(!KalshiBotGate::preregister(path, baseline_params(), kNow, &error).isEmpty());
        QVERIFY(!(QFileInfo(path).permissions() & QFileDevice::WriteOwner));
    }

    /// Refuse-to-edit-while-active: criteria cannot be re-sealed to fit the
    /// results they are about to judge.
    void preregister_refuses_to_reseal_over_existing_params() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-gate-params.json"));
        QString error;
        const QJsonObject first =
            KalshiBotGate::preregister(path, baseline_params(), kNow, &error);
        QVERIFY(!first.isEmpty());

        QJsonObject easier = baseline_params();
        easier.insert(QStringLiteral("min_settled_bids"), 1000);
        error.clear();
        QVERIFY(KalshiBotGate::preregister(path, easier, kNow + 1, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("immutable")));
        // The original file is untouched.
        const QJsonValue reloaded = KalshiBotGate::load_params_file(path);
        QCOMPARE(reloaded.toObject().value(QStringLiteral("seal_sha256")).toString(),
                 first.value(QStringLiteral("seal_sha256")).toString());
    }

    // --- the baseline: all four criteria met ------------------------------

    void baseline_ledger_passes_every_criterion() {
        const QJsonObject out = verdict_of(baseline_ledger());
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictPass));
        QVERIFY(out.value(QStringLiteral("evaluated")).toBool());
        assert_only_failure(out, "");
        const QJsonObject ledger = out.value(QStringLiteral("ledger")).toObject();
        QCOMPARE(ledger.value(QStringLiteral("settled_bids")).toInt(), 300);
        QCOMPARE(ledger.value(QStringLiteral("net_pnl_usd")).toDouble(), 30.0);
        QCOMPARE(ledger.value(QStringLiteral("max_drawdown_usd")).toDouble(), 0.0);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionBrier)
                     .value(QStringLiteral("brier_bot")).toDouble(), 0.0625);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionBrier)
                     .value(QStringLiteral("brier_market_baseline")).toDouble(), 0.25);
    }

    // --- one flip per criterion -------------------------------------------

    void one_bid_short_of_the_minimum_fails_only_the_count() {
        const QJsonObject out = verdict_of(baseline_ledger(299));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionSettled);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionSettled)
                     .value(QStringLiteral("observed")).toInt(), 299);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionSettled)
                     .value(QStringLiteral("required")).toInt(), 300);
    }

    void a_losing_record_fails_only_the_pnl_criterion() {
        // A cent lost per bid: $3.00 down over 300 bids, which is still well
        // inside the $50 drawdown limit, so only P&L flips.
        const QJsonObject out = verdict_of(baseline_ledger(300, 0.75, 0.50, -0.01));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionNetPnl);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionNetPnl)
                     .value(QStringLiteral("observed")).toDouble(), -3.0);
    }

    /// Breaking even is not passing: the criterion is strictly greater than.
    void a_flat_record_fails_the_pnl_criterion() {
        const QJsonObject out = verdict_of(baseline_ledger(300, 0.75, 0.50, 0.0));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionNetPnl);
    }

    void a_worse_than_market_forecast_fails_only_the_brier_criterion() {
        // The bot says 0.50 where the market says 0.75 and the contract
        // settles YES: bot Brier 0.25, market Brier 0.0625.
        const QJsonObject out = verdict_of(baseline_ledger(300, 0.50, 0.75));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionBrier);
        const QJsonObject brier = criterion(out, KalshiBotGate::kCriterionBrier);
        QCOMPARE(brier.value(QStringLiteral("brier_bot")).toDouble(), 0.25);
        QCOMPARE(brier.value(QStringLiteral("brier_market_baseline")).toDouble(), 0.0625);
        QCOMPARE(brier.value(QStringLiteral("scored_contracts")).toInt(), 300);
    }

    /// Matching the market is not beating it.
    void a_tied_brier_fails_the_brier_criterion() {
        const QJsonObject out = verdict_of(baseline_ledger(300, 0.60, 0.60));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionBrier);
    }

    /// A preregistered margin is a real hurdle: an improvement smaller than
    /// the margin fails even though the raw Brier is better.
    void an_improvement_smaller_than_the_sealed_margin_fails() {
        QJsonObject params = baseline_params();
        params.insert(QStringLiteral("min_brier_margin"), 0.20);
        const QJsonObject out = verdict_of(baseline_ledger(), sealed_record(params));
        // 0.0625 vs 0.25 is a 0.1875 improvement — short of the 0.20 margin.
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionBrier);
    }

    void an_early_losing_streak_fails_only_the_drawdown_criterion() {
        // 30 bids lose $2 each (a $60 trough), then 270 win $1 each: the
        // record ends $210 up with a Brier that beats the market, so the only
        // criterion left to fail is the drawdown limit.
        Ledger ledger;
        for (int i = 0; i < 30; ++i)
            append_settled_bid(ledger, i, 0.75, 0.50, QStringLiteral("YES"), -2.00);
        for (int i = 30; i < 300; ++i)
            append_settled_bid(ledger, i, 0.75, 0.50, QStringLiteral("YES"), 1.00);

        const QJsonObject out = verdict_of(ledger);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        assert_only_failure(out, KalshiBotGate::kCriterionDrawdown);
        QCOMPARE(criterion(out, KalshiBotGate::kCriterionDrawdown)
                     .value(QStringLiteral("observed")).toDouble(), 60.0);
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("net_pnl_usd")).toDouble(), 210.0);
    }

    /// Drawdown is peak-to-trough, not final-P&L: a mid-record dip counts even
    /// when the record recovers, and it is measured in settlement order.
    void drawdown_is_measured_peak_to_trough_in_settlement_order() {
        Ledger ledger;
        append_settled_bid(ledger, 0, 0.75, 0.50, QStringLiteral("YES"), 10.00);
        append_settled_bid(ledger, 1, 0.75, 0.50, QStringLiteral("YES"), -4.00);
        append_settled_bid(ledger, 2, 0.75, 0.50, QStringLiteral("YES"), -2.00);
        append_settled_bid(ledger, 3, 0.75, 0.50, QStringLiteral("YES"), 9.00);
        const QJsonObject out = verdict_of(ledger);
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("max_drawdown_usd")).toDouble(), 6.0);
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("net_pnl_usd")).toDouble(), 13.0);
    }

    // --- refusals: a gate that cannot trust its criteria never evaluates ---

    /// The alteration here loosens a gate from (500 bids, $5 drawdown) to
    /// (300, $50) — both perfectly valid criteria, so ONLY the seal can catch
    /// it. That is the edit the seal exists to prevent.
    void altered_params_refuse_with_tampered_and_publish_no_numbers() {
        QJsonObject record = sealed_record(QJsonObject{{QStringLiteral("min_settled_bids"), 500},
                                                       {QStringLiteral("max_drawdown_usd"), 5.0}});
        record.insert(QStringLiteral("params"), baseline_params());

        const QJsonObject out = verdict_of(baseline_ledger(), record);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictTampered));
        QVERIFY(!out.value(QStringLiteral("evaluated")).toBool());
        QVERIFY(!out.contains(QStringLiteral("criteria")));
        QVERIFY(!out.contains(QStringLiteral("ledger")));
        QVERIFY(!out.value(QStringLiteral("reason")).toString().isEmpty());
    }

    void unsealed_params_refuse_with_tampered() {
        QJsonObject record = sealed_record();
        record.remove(QStringLiteral("seal_sha256"));
        const QJsonObject out = verdict_of(baseline_ledger(), record);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictTampered));
        QVERIFY(!out.contains(QStringLiteral("criteria")));
    }

    void a_params_file_that_is_not_an_object_refuses_with_tampered() {
        const QJsonObject out = verdict_of(baseline_ledger(), QJsonValue(false));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictTampered));
        QVERIFY(!out.contains(QStringLiteral("criteria")));
    }

    /// Absent params are not tampering — but they are not a verdict either.
    void absent_params_refuse_without_evaluating() {
        const QJsonObject out = verdict_of(baseline_ledger(), QJsonValue(QJsonValue::Undefined));
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictNotPreregistered));
        QVERIFY(!out.value(QStringLiteral("evaluated")).toBool());
        QVERIFY(!out.contains(QStringLiteral("criteria")));
        QCOMPARE(KalshiBotGate::load_params_file(QStringLiteral("/nonexistent/gate-params.json")),
                 QJsonValue(QJsonValue::Undefined));
    }

    /// A correctly sealed file can still be an invalid gate: the floors are
    /// re-checked on every read, so a hand-sealed "min_settled_bids: 5" is
    /// refused instead of evaluated.
    void a_correctly_sealed_but_below_floor_gate_refuses() {
        QJsonObject record{{QStringLiteral("schema"), 1},
                           {QStringLiteral("gate_id"), QStringLiteral("hand-rolled")},
                           {QStringLiteral("params"),
                            QJsonObject{{QStringLiteral("min_settled_bids"), 5},
                                        {QStringLiteral("max_drawdown_usd"), 50.0}}}};
        record.insert(QStringLiteral("seal_sha256"), KalshiBotGate::seal(record));
        QVERIFY(KalshiBotGate::seal_valid(record));
        const QJsonObject out = verdict_of(baseline_ledger(10), record);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictTampered));
        QVERIFY(!out.contains(QStringLiteral("criteria")));
    }

    // --- no override path --------------------------------------------------

    /// There is no key that turns a FAIL into a PASS: an override is refused
    /// at preregistration, and one injected afterwards breaks the seal.
    void an_override_key_is_refused_at_seal_time_and_breaks_the_seal_afterwards() {
        QJsonObject with_override = baseline_params();
        with_override.insert(QStringLiteral("force_pass"), true);
        QString error;
        QVERIFY(KalshiBotGate::parse_params(with_override, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("force_pass")));

        QJsonObject record = sealed_record();
        record.insert(QStringLiteral("force_pass"), true);
        QVERIFY(!KalshiBotGate::seal_valid(record));
        const QJsonObject out = verdict_of(baseline_ledger(299), record);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictTampered));
    }

    // --- preregistration validation ---------------------------------------

    void params_below_the_ladder_floors_are_refused() {
        QString error;
        const auto refused = [&error](QJsonObject params) {
            error.clear();
            return KalshiBotGate::parse_params(params, &error).isEmpty() && !error.isEmpty();
        };
        QJsonObject params = baseline_params();

        params.insert(QStringLiteral("min_settled_bids"), 299);
        QVERIFY(refused(params)); // below the ladder's 300-bid floor
        params.insert(QStringLiteral("min_settled_bids"), 300.5);
        QVERIFY(refused(params)); // not an integer
        params.remove(QStringLiteral("min_settled_bids"));
        QVERIFY(refused(params)); // required

        params = baseline_params();
        params.insert(QStringLiteral("min_net_pnl_usd"), -1.0);
        QVERIFY(refused(params)); // a gate may not preregister a loss as success

        params = baseline_params();
        params.insert(QStringLiteral("max_drawdown_usd"), 0.0);
        QVERIFY(refused(params));
        params.insert(QStringLiteral("max_drawdown_usd"), 5000.0);
        QVERIFY(refused(params)); // vacuous under the $2/$3 caps
        params.remove(QStringLiteral("max_drawdown_usd"));
        QVERIFY(refused(params)); // required

        params = baseline_params();
        params.insert(QStringLiteral("min_brier_margin"), -0.01);
        QVERIFY(refused(params));
        params.insert(QStringLiteral("min_brier_margin"), 0.30);
        QVERIFY(refused(params));
    }

    void tightened_criteria_are_accepted() {
        QString error = QStringLiteral("untouched");
        const QJsonObject params =
            KalshiBotGate::parse_params(QJsonObject{{QStringLiteral("min_settled_bids"), 500},
                                                    {QStringLiteral("max_drawdown_usd"), 5.0},
                                                    {QStringLiteral("min_net_pnl_usd"), 10.0},
                                                    {QStringLiteral("min_brier_margin"), 0.01}},
                                        &error);
        QVERIFY2(!params.isEmpty(), qPrintable(error));
        QCOMPARE(params.value(QStringLiteral("min_settled_bids")).toInt(), 500);
        QCOMPARE(params.value(QStringLiteral("max_drawdown_usd")).toDouble(), 5.0);
    }

    // --- honest ledger arithmetic ------------------------------------------

    /// Nothing settled means nothing proven: the count fails, and the Brier is
    /// reported unavailable rather than as a flawless 0.0.
    void an_empty_ledger_fails_without_inventing_a_brier() {
        const QJsonObject out = verdict_of(Ledger{});
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        const QJsonObject brier = criterion(out, KalshiBotGate::kCriterionBrier);
        QVERIFY(!brier.value(QStringLiteral("brier_available")).toBool());
        QVERIFY(!brier.value(QStringLiteral("met")).toBool());
        QVERIFY(!brier.contains(QStringLiteral("observed")));
        QVERIFY(!brier.contains(QStringLiteral("brier_bot")));
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("settled_bids")).toInt(), 0);
        QVERIFY(out.value(QStringLiteral("ledger")).toObject()
                    .value(QStringLiteral("first_settled_ts_ms")).isNull());
    }

    /// A settlement whose opening decision is missing cannot be scored — it
    /// still counts as a settled bid and still moves P&L, but it contributes
    /// no probability to either Brier.
    void a_settlement_without_its_decision_row_is_counted_but_not_scored() {
        Ledger ledger = baseline_ledger(2);
        ledger.decisions.removeFirst();
        const QJsonObject out = verdict_of(ledger);
        const QJsonObject summary = out.value(QStringLiteral("ledger")).toObject();
        QCOMPARE(summary.value(QStringLiteral("settled_bids")).toInt(), 2);
        QCOMPARE(summary.value(QStringLiteral("scored_contracts")).toInt(), 1);
        QCOMPARE(summary.value(QStringLiteral("unscored_contracts")).toInt(), 1);
    }

    void duplicate_and_malformed_settlement_rows_are_not_counted_as_settled_bids() {
        Ledger ledger = baseline_ledger(2);
        ledger.settlements.append(ledger.settlements.first()); // replayed row
        QJsonObject no_pnl = ledger.settlements.last().toObject();
        no_pnl.remove(QStringLiteral("realized_pnl"));
        no_pnl.insert(QStringLiteral("position_id"), QStringLiteral("ORPHAN@1"));
        ledger.settlements.append(no_pnl);
        ledger.settlements.append(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
            {QStringLiteral("realized_pnl"), 99.0}}); // no position_id: unidentifiable

        const QJsonObject summary =
            verdict_of(ledger).value(QStringLiteral("ledger")).toObject();
        QCOMPARE(summary.value(QStringLiteral("settled_bids")).toInt(), 2);
        QCOMPARE(summary.value(QStringLiteral("net_pnl_usd")).toDouble(), 0.20);
        QCOMPARE(summary.value(QStringLiteral("malformed_settlement_rows")).toInt(), 2);
    }

    // --- the record must be WHOLE before it is scored (issue #152) ---------
    //
    // A truncated record is indistinguishable from a shorter one once it is
    // just an array of rows, so `evaluate()` is told what the caller could see
    // of the record on disk. Both checks refuse in the same shape a tampered
    // params file does: no criteria, no ledger numbers, a named reason.

    void a_hole_in_the_generation_sequence_is_refused_not_scored() {
        KalshiBotGate::RecordIntegrity holed;
        holed.missing_generations =
            QStringList{QStringLiteral("/evidence/kalshi-bot-decisions.jsonl.2")};
        holed.oldest_row_ts_ms = kNow;

        const QJsonObject out = verdict_of(baseline_ledger(), sealed_record(), holed);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictRecordIncomplete));
        QCOMPARE(out.value(QStringLiteral("evaluated")).toBool(), false);
        QVERIFY2(!out.contains(QStringLiteral("criteria")), "a refusal published criteria");
        QVERIFY2(!out.contains(QStringLiteral("ledger")),
                 "a refusal published ledger numbers about a record it did not score");
        QVERIFY(out.value(QStringLiteral("reason")).toString().contains(
            QStringLiteral("kalshi-bot-decisions.jsonl.2")));

        // The control: the very same rows, contiguous, are a normal verdict.
        QCOMPARE(verdict(verdict_of(baseline_ledger())),
                 QString::fromLatin1(KalshiBotGate::kVerdictPass));
    }

    void a_record_that_begins_after_the_published_anchor_is_refused() {
        // The truncation a two-generation recycling rotation leaves behind: a
        // perfectly contiguous record whose beginning is gone. Only the anchor
        // already on disk can see it.
        KalshiBotGate::RecordIntegrity truncated;
        truncated.oldest_row_ts_ms = kNow + 5'000;
        truncated.published_first_settled_ts_ms = kNow + 1'000;

        const QJsonObject out = verdict_of(baseline_ledger(), sealed_record(), truncated);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictRecordIncomplete));
        QVERIFY(!out.contains(QStringLiteral("ledger")));
        const QString reason = out.value(QStringLiteral("reason")).toString();
        QVERIFY2(reason.contains(QString::number(kNow + 5'000)), qPrintable(reason));
        QVERIFY2(reason.contains(QString::number(kNow + 1'000)), qPrintable(reason));
    }

    void a_record_that_still_covers_the_published_anchor_is_scored() {
        // The state the operator's evidence dir is in if the OLD rotation fires
        // before this lands: `.1` + base together are the whole record, and the
        // multi-generation reader repairs it. It must score, not refuse.
        KalshiBotGate::RecordIntegrity whole;
        whole.oldest_row_ts_ms = kNow;                      // the first decision row
        whole.published_first_settled_ts_ms = kNow + 1'000; // the first settlement

        const QJsonObject out = verdict_of(baseline_ledger(), sealed_record(), whole);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictPass));
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject(),
                 verdict_of(baseline_ledger()).value(QStringLiteral("ledger")).toObject());
    }

    void a_record_with_no_dated_row_at_all_is_refused_once_something_was_scored() {
        KalshiBotGate::RecordIntegrity gone;
        gone.published_first_settled_ts_ms = kNow + 1'000;
        QCOMPARE(verdict(verdict_of(Ledger(), sealed_record(), gone)),
                 QString::fromLatin1(KalshiBotGate::kVerdictRecordIncomplete));

        // But a fresh install — an empty record with nothing ever published —
        // is not a truncation. It is judged, and it fails on its merits.
        QCOMPARE(verdict(verdict_of(Ledger())), QString::fromLatin1(KalshiBotGate::kVerdictFail));
    }

    /// The anchor is the ONLY thing a refusal carries forward, and it has to
    /// survive being published over the file it came from: without that, the
    /// first refusal erases the anchor and the next run scores the remainder.
    void the_anchor_survives_republication_through_a_refusal() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-gate.json"));
        const auto publish = [&path](const QJsonObject& out) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
            file.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
        };
        const auto published = [&path]() {
            QFile file(path);
            return file.open(QIODevice::ReadOnly | QIODevice::Text)
                       ? QJsonDocument::fromJson(file.readAll()).object()
                       : QJsonObject();
        };

        // 1. a normal verdict is published, and it carries the anchor.
        publish(verdict_of(baseline_ledger()));
        QCOMPARE(KalshiBotGate::published_anchor_ms(published()), kNow + 1'000);

        // 2. the record is truncated; the gate refuses and publishes the
        //    refusal OVER that verdict.
        KalshiBotGate::RecordIntegrity truncated;
        truncated.oldest_row_ts_ms = kNow + 5'000;
        truncated.published_first_settled_ts_ms = KalshiBotGate::published_anchor_ms(published());
        const QJsonObject refusal = verdict_of(baseline_ledger(), sealed_record(), truncated);
        QCOMPARE(verdict(refusal), QString::fromLatin1(KalshiBotGate::kVerdictRecordIncomplete));
        publish(refusal);

        // 3. the next run reads the anchor back out of that refusal and
        //    refuses again, instead of scoring the remainder.
        QCOMPARE(KalshiBotGate::published_anchor_ms(published()), kNow + 1'000);
        truncated.published_first_settled_ts_ms = KalshiBotGate::published_anchor_ms(published());
        QCOMPARE(verdict(verdict_of(baseline_ledger(), sealed_record(), truncated)),
                 QString::fromLatin1(KalshiBotGate::kVerdictRecordIncomplete));
    }

    void a_verdict_that_scored_nothing_carries_no_anchor() {
        // `first_settled_ts_ms` is null whenever nothing settled: that is "no
        // anchor", never an anchor at the epoch that would refuse every record.
        QVERIFY(verdict_of(Ledger()).value(QStringLiteral("ledger")).toObject()
                    .value(QStringLiteral("first_settled_ts_ms")).isNull());
        QCOMPARE(KalshiBotGate::published_anchor_ms(verdict_of(Ledger())), 0);
        QCOMPARE(KalshiBotGate::published_anchor_ms({}), 0);
    }

    /// Decision rows are not settlements: passes and unsettled bids move
    /// nothing, so a ledger of open positions cannot pass the gate.
    void unsettled_bids_and_passes_do_not_count_as_settled() {
        Ledger ledger = baseline_ledger(300);
        ledger.settlements = QJsonArray();
        const QJsonObject out = verdict_of(ledger);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("settled_bids")).toInt(), 0);
    }

    // ---- quarantined evidence -------------------------------------------
    // Trust was once POOLED: one flag over KXGOLDH + KXSILVERH + KXWTIH
    // authorised bids in all three from evidence no single one had earned.
    // Five KXGOLDH settlements exist because of it. They are real outcomes and
    // they stay in the append-only ledger forever -- but they are not gold's
    // evidence, so they must not count toward gold's promotion.
    void quarantined_settlements_do_not_count_toward_promotion() {
        Ledger ledger = baseline_ledger();
        const QString id = ledger.settlements.at(0).toObject()
                               .value(QStringLiteral("position_id")).toString();
        QVERIFY(!id.isEmpty());

        const QJsonObject clean = verdict_of(ledger);
        QCOMPARE(clean.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("settled_bids")).toInt(), 300);

        const QJsonObject quarantined = KalshiBotGate::evaluate(
            sealed_record(), ledger.decisions, ledger.settlements, kNow,
            KalshiBotGate::RecordIntegrity::whole(), QSet<QString>{id});
        const QJsonObject led = quarantined.value(QStringLiteral("ledger")).toObject();
        QCOMPARE(led.value(QStringLiteral("settled_bids")).toInt(), 299);
        QCOMPARE(led.value(QStringLiteral("quarantined_settlements")).toInt(), 1);
        // Quarantine must be VISIBLE, not a silent subtraction: a record that
        // quietly shrank would be indistinguishable from a truncated one.
        QVERIFY(led.contains(QStringLiteral("quarantined_settlements")));
    }

    // Quarantine can only ever make the gate HARDER to pass, never easier --
    // it removes evidence, and the settled floor is a minimum.
    void quarantine_can_only_tighten_the_gate() {
        Ledger ledger = baseline_ledger(300);
        QSet<QString> ids;
        for (int i = 0; i < 5; ++i)
            ids.insert(ledger.settlements.at(i).toObject()
                           .value(QStringLiteral("position_id")).toString());

        QCOMPARE(verdict(verdict_of(ledger)), QString::fromLatin1(KalshiBotGate::kVerdictPass));
        const QJsonObject out = KalshiBotGate::evaluate(
            sealed_record(), ledger.decisions, ledger.settlements, kNow,
            KalshiBotGate::RecordIntegrity::whole(), ids);
        QCOMPARE(verdict(out), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        QCOMPARE(out.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("settled_bids")).toInt(), 295);
    }

    // The reader takes only rows that say what they are, and a row naming no
    // position quarantines nothing -- treating it as a wildcard would erase
    // the record it exists to mark.
    void the_quarantine_reader_ignores_rows_that_name_nothing() {
        const QJsonArray rows{
            QJsonObject{{QStringLiteral("event"),
                         QString::fromLatin1(KalshiBotGate::kQuarantineEvent)},
                        {QStringLiteral("position_id"), QStringLiteral("KXGOLDH-A@1")}},
            QJsonObject{{QStringLiteral("event"),
                         QString::fromLatin1(KalshiBotGate::kQuarantineEvent)},
                        {QStringLiteral("position_id"), QStringLiteral("   ")}},
            QJsonObject{{QStringLiteral("event"), QStringLiteral("something_else")},
                        {QStringLiteral("position_id"), QStringLiteral("KXGOLDH-B@2")}},
        };
        const QSet<QString> ids = KalshiBotGate::quarantined_position_ids(rows);
        QCOMPARE(ids.size(), 1);
        QVERIFY(ids.contains(QStringLiteral("KXGOLDH-A@1")));
    }

};

QTEST_APPLESS_MAIN(TestKalshiBotGate)
#include "tst_kalshi_bot_gate.moc"
