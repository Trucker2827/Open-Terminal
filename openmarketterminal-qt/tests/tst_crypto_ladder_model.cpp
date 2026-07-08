#include "screens/crypto_trading/CryptoLadderModel.h"
#include <QtTest>
using namespace openmarketterminal::crypto;

class TestCryptoLadderModel : public QObject {
    Q_OBJECT
  private slots:
    void bucketsPrices();
    void buildBucketsDepthAroundMid();
};

void TestCryptoLadderModel::bucketsPrices() {
    QCOMPARE(bucket_price(62617.3, 10.0), 62610.0);
    QCOMPARE(bucket_price(62610.0, 10.0), 62610.0);
    QCOMPARE(bucket_price(3.27, 0.1), 3.2); // fuzzy-safe multiple
    QVERIFY(qAbs(bucket_price(3.27, 0.1) - 3.2) < 1e-9);
}

void TestCryptoLadderModel::buildBucketsDepthAroundMid() {
    // bids below, asks above; two raw levels in the 62,600 bucket sum.
    QVector<QPair<double,double>> bids{{62599, 2.0}, {62595, 3.0}, {62580, 1.0}};
    QVector<QPair<double,double>> asks{{62601, 1.0}, {62603, 4.0}, {62620, 2.0}};
    CryptoLadderModel m;
    const auto v = m.build(bids, asks, 10.0, 2);
    // mid = (best_bid 62599 + best_ask 62601)/2 = 62600
    QVERIFY(qAbs(v.mid - 62600.0) < 1e-6);
    // rows are 2 each side of the 62600 bucket, top->bottom descending price
    QCOMPARE(v.rows.size(), 5); // 62620,62610,62600,62590,62580
    QCOMPARE(v.rows.first().price, 62620.0);
    QCOMPARE(v.rows.last().price, 62580.0);
    // 62600 bucket: asks 62601(1)+62603(4)=5 on the ask side; no bid there
    auto row600 = std::find_if(v.rows.begin(), v.rows.end(),
                               [](const LadderRow& r){ return qAbs(r.price-62600.0)<1e-6; });
    QVERIFY(row600 != v.rows.end());
    QCOMPARE(row600->ask_size, 5.0);
    QCOMPARE(row600->bid_size, 0.0);
    // 62590 bucket has bids 62599? no -> 62599 buckets to 62590: 2.0; 62595->62590: 3.0 => 5.0
    auto row590 = std::find_if(v.rows.begin(), v.rows.end(),
                               [](const LadderRow& r){ return qAbs(r.price-62590.0)<1e-6; });
    QCOMPARE(row590->bid_size, 5.0);
    QCOMPARE(v.max_depth, 5.0);
}

QTEST_APPLESS_MAIN(TestCryptoLadderModel)
#include "tst_crypto_ladder_model.moc"
