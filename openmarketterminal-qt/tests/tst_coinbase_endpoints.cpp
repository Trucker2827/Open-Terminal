#include "services/crypto/CoinbaseEndpoints.h"
#include <QtTest/QtTest>

#include <QFile>
#include <QJsonDocument>

using namespace openmarketterminal::services::crypto;

namespace {
QJsonDocument load_fixture(const QString& name) {
    QFile f(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/") + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll());
}
} // namespace

class TstCoinbaseEndpoints : public QObject {
    Q_OBJECT
  private slots:
    void ticker_url() {
        const QString u = advanced_ticker_url(QStringLiteral("BTC-USD")).toString();
        QVERIFY(u.startsWith("https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD/ticker"));
        QVERIFY(u.contains("limit=1"));
    }
    void candles_url() {
        const QString u = advanced_candles_url(QStringLiteral("BTC-USD"), 1700000000, 1700000600).toString();
        QVERIFY(u.startsWith("https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD/candles"));
        QVERIFY(u.contains("granularity=ONE_MINUTE"));
        QVERIFY(u.contains("start=1700000000"));
        QVERIFY(u.contains("end=1700000600"));
    }
    void parse_ticker_fixture() {
        const QJsonDocument doc = load_fixture(QStringLiteral("coinbase_adv_ticker.json"));
        QVERIFY(!doc.isNull());
        double price = 0, bid = 0, ask = 0;
        QVERIFY(parse_advanced_ticker(doc, &price, &bid, &ask));
        QVERIFY(price > 0);
        QVERIFY(bid > 0);
        QVERIFY(ask > 0);
        QVERIFY(bid <= ask);
    }
    void parse_candles_fixture() {
        const QJsonDocument doc = load_fixture(QStringLiteral("coinbase_adv_candles.json"));
        QVERIFY(!doc.isNull());
        const auto candles = parse_advanced_candles(doc);
        QVERIFY(candles.size() >= 1);
        for (const auto& c : candles) {
            QVERIFY(c.open_ms > 1000000000000LL); // ms-scaled epoch, not seconds
            QVERIFY(c.low <= c.high);
            QVERIFY(c.open > 0);
            QVERIFY(c.close > 0);
            QVERIFY(c.volume >= 0);
        }
    }
    void parse_ticker_rejects_garbage() {
        double price = 0, bid = 0, ask = 0;
        QVERIFY(!parse_advanced_ticker(QJsonDocument(), &price, &bid, &ask));
        QVERIFY(!parse_advanced_ticker(QJsonDocument::fromJson("{\"trades\":[]}"), &price, &bid, &ask));
    }
    void subscribe_frame() {
        const QJsonObject f = advanced_ws_subscribe({QStringLiteral("BTC-USD")}, QStringLiteral("level2"));
        QCOMPARE(f.value("type").toString(), QString("subscribe"));
        QCOMPARE(f.value("channel").toString(), QString("level2")); // SINGLE channel string
        QVERIFY(!f.contains("channels"));                           // never the legacy array form
        const QJsonArray ids = f.value("product_ids").toArray();
        QCOMPARE(ids.size(), 1);
        QCOMPARE(ids.at(0).toString(), QString("BTC-USD"));
    }
};

QTEST_MAIN(TstCoinbaseEndpoints)
#include "tst_coinbase_endpoints.moc"
