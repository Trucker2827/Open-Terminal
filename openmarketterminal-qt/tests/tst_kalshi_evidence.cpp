#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
#include "services/edge_radar/KalshiUniversalEdgeModel.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal::services::prediction;
using openmarketterminal::services::prediction::kalshi_ns::KalshiEvidenceEngine;

namespace {

PredictionMarket market(const QString& ticker, const QString& event, const QString& kind,
                        double floor, double cap, double yes_bid, double no_bid) {
    PredictionMarket m;
    m.key.exchange_id = QStringLiteral("kalshi");
    m.key.market_id = ticker;
    m.key.event_id = event;
    m.key.asset_ids = {ticker + QStringLiteral(":yes"), ticker + QStringLiteral(":no")};
    m.outcomes = {{QStringLiteral("Yes"), m.key.asset_ids[0], yes_bid},
                  {QStringLiteral("No"), m.key.asset_ids[1], no_bid}};
    m.extras.insert(QStringLiteral("floor_strike"), floor);
    m.extras.insert(QStringLiteral("cap_strike"), cap);
    m.extras.insert(QStringLiteral("strike_type"), kind == QStringLiteral("above")
                                                        ? QStringLiteral("greater")
                                                        : QStringLiteral("between"));
    return m;
}

PredictionOrderBook book(const QString& asset, double bid, double ask) {
    PredictionOrderBook b;
    b.asset_id = asset;
    b.bids = {{bid, 100.0}};
    b.asks = {{ask, 100.0}};
    b.last_update_ms = 1;
    return b;
}

bool has_kind(const QJsonArray& rows, const QString& kind) {
    for (const auto& value : rows)
        if (value.toObject().value(QStringLiteral("kind")).toString() == kind) return true;
    return false;
}

QString severity_for(const QJsonArray& rows, const QString& kind) {
    for (const auto& value : rows) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("kind")).toString() == kind)
            return row.value(QStringLiteral("severity")).toString();
    }
    return {};
}

void write_feature(QFile& file, const QDateTime& ts, double mid, const QString& market_id) {
    QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_venue_feature_snapshot")},
                    {QStringLiteral("ts"), ts.toUTC().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("symbol"), QStringLiteral("BTC/USD")},
                    {QStringLiteral("kalshi_market_id"), market_id},
                    {QStringLiteral("reference_mid"), mid},
                    {QStringLiteral("kalshi_yes_price"), 0.50 + (mid - 100.0) / 100.0}};
    file.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    file.write("\n");
}

} // namespace

class TestKalshiEvidence : public QObject {
    Q_OBJECT
  private slots:
    void detectsExecutableRelationships();
    void reconcilesForwardAndSettlementLabels();
    void appliesSeriesFeesAndWaivers();
    void pricesExecutableCryptoYesAndNo();
    void rejectsLateOrFutureOnlyCryptoInputs();
    void formatsCalibratorReadoutWithHonestRecord();
    void withholdsMissingOrStaleCalibratorNumbers();
};

void TestKalshiEvidence::detectsExecutableRelationships() {
    const QString event = QStringLiteral("BTC-HOUR");
    QVector<PredictionMarket> markets{
        market(QStringLiteral("LOW"), event, QStringLiteral("above"), 100.0, 0.0, 0.39, 0.58),
        market(QStringLiteral("RANGE"), event, QStringLiteral("range"), 100.0, 110.0, 0.18, 0.77),
        market(QStringLiteral("HIGH"), event, QStringLiteral("above"), 110.0, 0.0, 0.59, 0.39),
    };
    QHash<QString, PredictionOrderBook> books;
    books.insert(QStringLiteral("LOW:yes"), book(QStringLiteral("LOW:yes"), 0.39, 0.40));
    books.insert(QStringLiteral("LOW:no"), book(QStringLiteral("LOW:no"), 0.58, 0.05));
    books.insert(QStringLiteral("RANGE:yes"), book(QStringLiteral("RANGE:yes"), 0.18, 0.05));
    books.insert(QStringLiteral("RANGE:no"), book(QStringLiteral("RANGE:no"), 0.77, 0.80));
    books.insert(QStringLiteral("HIGH:yes"), book(QStringLiteral("HIGH:yes"), 0.59, 0.60));
    books.insert(QStringLiteral("HIGH:no"), book(QStringLiteral("HIGH:no"), 0.39, 0.40));

    const QJsonArray diagnostics = KalshiEvidenceEngine::analyze_ladder(markets, books, event);
    QVERIFY(has_kind(diagnostics, QStringLiteral("monotonicity_violation")));
    QVERIFY(has_kind(diagnostics, QStringLiteral("nested_above_portfolio")));
    QVERIFY(has_kind(diagnostics, QStringLiteral("range_partition_portfolio")));
    QCOMPARE(severity_for(diagnostics, QStringLiteral("nested_above_portfolio")),
             QStringLiteral("opportunity"));

    books[QStringLiteral("LOW:yes")].asks[0].size = 0.0;
    const QJsonArray no_depth = KalshiEvidenceEngine::analyze_ladder(markets, books, event);
    QCOMPARE(severity_for(no_depth, QStringLiteral("nested_above_portfolio")),
             QStringLiteral("watch"));
}

void TestKalshiEvidence::appliesSeriesFeesAndWaivers() {
    auto priced = market(QStringLiteral("MKT"), QStringLiteral("BTC-HOUR"),
                         QStringLiteral("above"), 100.0, 0.0, 0.0, 0.0);
    QCOMPARE(KalshiEvidenceEngine::conservative_taker_fee(priced, 0.50, 10.0), 0.18);
    priced.extras.insert(QStringLiteral("fee_multiplier"), 2.0);
    QCOMPARE(KalshiEvidenceEngine::conservative_taker_fee(priced, 0.50, 10.0), 0.35);
    priced.extras.insert(QStringLiteral("fee_multiplier"), 0.0);
    QCOMPARE(KalshiEvidenceEngine::conservative_taker_fee(priced, 0.50, 10.0), 0.0);
    priced.extras.insert(QStringLiteral("fee_multiplier"), 2.0);
    priced.extras.insert(QStringLiteral("fee_waiver_expiration_time"),
                         QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate));
    QCOMPARE(KalshiEvidenceEngine::conservative_taker_fee(priced, 0.50, 10.0), 0.0);
}

void TestKalshiEvidence::reconcilesForwardAndSettlementLabels() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString features = dir.filePath(QStringLiteral("features.jsonl"));
    const QString labels = dir.filePath(QStringLiteral("labels.jsonl"));
    QFile file(features);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    const QDateTime base = QDateTime::fromString(QStringLiteral("2026-07-11T10:00:00Z"), Qt::ISODate);
    write_feature(file, base, 100.0, QStringLiteral("MKT"));
    write_feature(file, base.addSecs(15), 101.0, QStringLiteral("MKT"));
    write_feature(file, base.addSecs(60), 102.0, QStringLiteral("MKT"));
    write_feature(file, base.addSecs(250), 102.5, QStringLiteral("MKT"));
    write_feature(file, base.addSecs(300), 103.0, QStringLiteral("MKT"));
    file.close();

    QCOMPARE(KalshiEvidenceEngine::reconcile_forward_labels(features, labels), 1);
    QCOMPARE(KalshiEvidenceEngine::reconcile_forward_labels(features, labels), 0);
    QFile labels_file(labels);
    QVERIFY(labels_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto label = QJsonDocument::fromJson(labels_file.readLine()).object();
    QCOMPARE(label.value(QStringLiteral("spot_15s_bps")).toDouble(), 100.0);
    QCOMPARE(label.value(QStringLiteral("spot_1m_bps")).toDouble(), 200.0);
    QCOMPARE(label.value(QStringLiteral("spot_5m_bps")).toDouble(), 300.0);

    auto settled = market(QStringLiteral("MKT"), QStringLiteral("BTC-HOUR"),
                          QStringLiteral("above"), 100.0, 0.0, 0.0, 0.0);
    settled.end_date_iso = base.addSecs(300).toString(Qt::ISODate);
    settled.extras.insert(QStringLiteral("expiration_value"), QStringLiteral("104.0"));
    settled.extras.insert(QStringLiteral("settlement_value_dollars"), 1.0);
    settled.extras.insert(QStringLiteral("result"), QStringLiteral("yes"));
    const auto settlement = KalshiEvidenceEngine::settlement_label(settled, features);
    QCOMPARE(settlement.value(QStringLiteral("proxy_samples")).toInt(), 2);
    QVERIFY(settlement.contains(QStringLiteral("basis_error_usd")));
}

void TestKalshiEvidence::pricesExecutableCryptoYesAndNo() {
    using namespace openmarketterminal::services::edge_radar;
    const qint64 now = QDateTime::fromString(QStringLiteral("2026-07-12T12:00:00Z"), Qt::ISODate)
                           .toMSecsSinceEpoch();
    auto priced = market(QStringLiteral("KXBTC-HOURLY"), QStringLiteral("BTC-HOUR"),
                         QStringLiteral("above"), 64000.0, 0.0, 0.48, 0.49);
    priced.question = QStringLiteral("BTC price above $64,000 in one hour?");
    priced.category = QStringLiteral("Crypto");
    priced.end_date_iso = QStringLiteral("2026-07-12T13:00:00Z");
    priced.active = true;
    priced.extras.insert(QStringLiteral("yes_ask_dollars"), 0.50);
    priced.extras.insert(QStringLiteral("no_ask_dollars"), 0.52);
    priced.liquidity = 20000.0;

    KalshiUniversalOptions options;
    options.minimum_net_edge = 0.01;
    options.minimum_seconds_left = 20;
    const auto yes = KalshiUniversalEdgeModel::score_crypto_target(priced, 64500.0, options, now);
    QCOMPARE(yes.side, QStringLiteral("yes"));
    QCOMPARE(yes.market_probability, 0.50);
    QVERIFY(yes.model_probability > 0.50);
    QVERIFY(yes.seconds_left == 3600);
    QVERIFY(yes.reference_price == 64500.0);
    QVERIFY(yes.target_price == 64000.0);

    const auto no = KalshiUniversalEdgeModel::score_crypto_target(priced, 63500.0, options, now);
    QCOMPARE(no.side, QStringLiteral("no"));
    QCOMPARE(no.market_probability, 0.52);
    QVERIFY(no.model_probability > 0.50);
}

void TestKalshiEvidence::rejectsLateOrFutureOnlyCryptoInputs() {
    using namespace openmarketterminal::services::edge_radar;
    const qint64 decision = QDateTime::fromString(QStringLiteral("2026-07-12T12:59:50Z"), Qt::ISODate)
                                .toMSecsSinceEpoch();
    auto priced = market(QStringLiteral("KXETH-HOURLY"), QStringLiteral("ETH-HOUR"),
                         QStringLiteral("above"), 1800.0, 0.0, 0.49, 0.49);
    priced.question = QStringLiteral("ETH price above $1,800 in one hour?");
    priced.category = QStringLiteral("Crypto");
    priced.end_date_iso = QStringLiteral("2026-07-12T13:00:00Z");
    priced.extras.insert(QStringLiteral("yes_ask_dollars"), 0.50);
    priced.extras.insert(QStringLiteral("no_ask_dollars"), 0.50);
    priced.liquidity = 20000.0;

    KalshiUniversalOptions options;
    options.minimum_net_edge = 0.0;
    options.minimum_seconds_left = 20;
    const auto late = KalshiUniversalEdgeModel::score_crypto_target(priced, 1810.0, options, decision);
    QVERIFY(!late.passes_gate);
    QVERIFY(late.rejection_reasons.contains(QStringLiteral("too late")));

    // A decision timestamp after close cannot borrow the earlier, favorable
    // state. It deterministically has zero seconds left and remains rejected.
    const auto after = KalshiUniversalEdgeModel::score_crypto_target(
        priced, 1900.0, options, decision + 60'000);
    QCOMPARE(after.seconds_left, 0);
    QVERIFY(!after.passes_gate);
}

namespace {

QJsonObject calibrator_report(qint64 generated_ms, bool adds_value, double per_min_vol_bps) {
    // Mirrors what spot_calibrator.py build_report() writes to calibrator.json.
    const QJsonObject features{{QStringLiteral("required_move_sigma"), 26.771822853615074},
                               {QStringLiteral("per_min_vol_bps"), per_min_vol_bps},
                               {QStringLiteral("signed_distance_bps"), 14.3},
                               {QStringLiteral("sqrt_minutes_left"), 0.258},
                               {QStringLiteral("realized_move_bps"), 0.0},
                               {QStringLiteral("yes_mid"), 0.0035}};
    const QJsonObject prediction{{QStringLiteral("p_yes_full"), 0.2690958555948577},
                                 {QStringLiteral("p_yes_market_baseline"), 0.0206},
                                 {QStringLiteral("market_yes_mid"), 0.0035},
                                 {QStringLiteral("features"), features}};
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QStringLiteral("spot_calibrator")},
                       {QStringLiteral("advisory_only"), true},
                       {QStringLiteral("generated_at_ms"), generated_ms},
                       {QStringLiteral("resolved_contracts"), 162},
                       {QStringLiteral("brier_full"), 0.109},
                       {QStringLiteral("brier_market_baseline"), 0.102},
                       {QStringLiteral("adds_value_over_market"), adds_value},
                       {QStringLiteral("predictions"),
                        QJsonObject{{QStringLiteral("KXBTC15M-26JUL230800-00"), prediction}}}};
}

} // namespace

void TestKalshiEvidence::formatsCalibratorReadoutWithHonestRecord() {
    const qint64 now = 1'784'847'600'000;
    const QString ticker = QStringLiteral("KXBTC15M-26JUL230800-00");

    // Round-trip through the JSON serializer, exactly like the file on disk.
    const QJsonObject parsed = QJsonDocument::fromJson(
        QJsonDocument(calibrator_report(now - 60'000, false, 2.069)).toJson()).object();
    const QJsonObject readout =
        KalshiEvidenceEngine::calibrator_readout(parsed, ticker, now);
    QCOMPARE(readout.value(QStringLiteral("state")).toString(), QStringLiteral("ok"));
    const QString headline = readout.value(QStringLiteral("headline")).toString();
    QVERIFY(headline.contains(QStringLiteral("26.8σ")));
    QVERIFY(headline.contains(QStringLiteral("P(YES) 26.9%")));
    QVERIFY(headline.contains(QStringLiteral("MARKET MID 0.4%")));
    const QString record = readout.value(QStringLiteral("record")).toString();
    QVERIFY(record.contains(QStringLiteral("162 resolved")));
    QVERIFY(record.contains(QStringLiteral("Brier 0.109 vs market 0.102")));
    QVERIFY(record.contains(QStringLiteral("does NOT beat market — opinion, not signal")));
    QCOMPARE(readout.value(QStringLiteral("trusted")).toBool(), false);

    const QJsonObject beats = KalshiEvidenceEngine::calibrator_readout(
        calibrator_report(now - 60'000, true, 2.069), ticker, now);
    QVERIFY(beats.value(QStringLiteral("record")).toString()
                .contains(QStringLiteral("beats market baseline")));
    QCOMPARE(beats.value(QStringLiteral("trusted")).toBool(), true);

    // A zero ambient vol makes required_move_sigma meaningless; the readout
    // must say so instead of presenting 26.8σ as a measurement.
    const QJsonObject no_vol = KalshiEvidenceEngine::calibrator_readout(
        calibrator_report(now - 60'000, false, 0.0), ticker, now);
    const QString no_vol_headline = no_vol.value(QStringLiteral("headline")).toString();
    QVERIFY(!no_vol_headline.contains(QStringLiteral("26.8σ")));
    QVERIFY(no_vol_headline.contains(QStringLiteral("σ UNAVAILABLE")));
}

void TestKalshiEvidence::withholdsMissingOrStaleCalibratorNumbers() {
    const qint64 now = 1'784'847'600'000;
    const QString ticker = QStringLiteral("KXBTC15M-26JUL230800-00");

    const QJsonObject missing_file =
        KalshiEvidenceEngine::calibrator_readout(QJsonObject{}, ticker, now);
    QCOMPARE(missing_file.value(QStringLiteral("state")).toString(), QStringLiteral("missing"));
    QVERIFY(missing_file.value(QStringLiteral("headline")).toString()
                .contains(QStringLiteral("MISSING")));

    const QJsonObject no_entry = KalshiEvidenceEngine::calibrator_readout(
        calibrator_report(now - 60'000, false, 2.069), QStringLiteral("KXETH-OTHER"), now);
    QCOMPARE(no_entry.value(QStringLiteral("state")).toString(), QStringLiteral("missing"));
    QVERIFY(no_entry.value(QStringLiteral("headline")).toString()
                .contains(QStringLiteral("no prediction")));

    // 16 minutes old is past the 15-minute default: state flips to stale and
    // every number is withheld — a stale probability never renders as live.
    const QJsonObject stale = KalshiEvidenceEngine::calibrator_readout(
        calibrator_report(now - 16 * 60'000, false, 2.069), ticker, now);
    QCOMPARE(stale.value(QStringLiteral("state")).toString(), QStringLiteral("stale"));
    const QString stale_headline = stale.value(QStringLiteral("headline")).toString();
    QVERIFY(stale_headline.contains(QStringLiteral("STALE")));
    QVERIFY(stale_headline.contains(QStringLiteral("16 min old")));
    QVERIFY(!stale_headline.contains(QStringLiteral("26.8")));
    QVERIFY(stale.value(QStringLiteral("record")).toString().isEmpty());

    // One minute inside the threshold still reads as live.
    const QJsonObject fresh = KalshiEvidenceEngine::calibrator_readout(
        calibrator_report(now - 14 * 60'000, false, 2.069), ticker, now);
    QCOMPARE(fresh.value(QStringLiteral("state")).toString(), QStringLiteral("ok"));
}

QTEST_APPLESS_MAIN(TestKalshiEvidence)
#include "tst_kalshi_evidence.moc"
