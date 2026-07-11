#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

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

QTEST_APPLESS_MAIN(TestKalshiEvidence)
#include "tst_kalshi_evidence.moc"
