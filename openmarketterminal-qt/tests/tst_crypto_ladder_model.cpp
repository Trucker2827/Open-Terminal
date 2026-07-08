#include "screens/crypto_trading/CryptoLadderModel.h"
#include <QtTest>
using namespace openmarketterminal::crypto;

class TestCryptoLadderModel : public QObject {
    Q_OBJECT
  private slots:
    void bucketsPrices();
    void buildBucketsDepthAroundMid();
    void buildBucketsFractionalGrouping();
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

void TestCryptoLadderModel::buildBucketsFractionalGrouping() {
    // Fractional grouping (0.1) regression: accumulation key and row-lookup
    // key must be bit-identical, or a lookup miss silently returns 0 depth.
    // Two raw bid levels fall in the 2.9 bucket (idx 29); two raw ask
    // levels fall in the 3.0 bucket (idx 30), so both sums are meaningful
    // (non-zero, non-trivial) checks on the accumulation+lookup path.
    QVector<QPair<double,double>> bids{{2.94, 1.0}, {2.98, 2.0}};
    QVector<QPair<double,double>> asks{{3.02, 3.0}, {3.06, 1.0}};

    // Hand-verify against bucket_index before asserting on build():
    QCOMPARE(bucket_index(2.94, 0.1), qint64(29));
    QCOMPARE(bucket_index(2.98, 0.1), qint64(29));
    QCOMPARE(bucket_index(3.02, 0.1), qint64(30));
    QCOMPARE(bucket_index(3.06, 0.1), qint64(30));

    CryptoLadderModel m;
    const auto v = m.build(bids, asks, 0.1, 1);
    // mid = (best_bid 2.98 + best_ask 3.02) / 2 = 3.00 -> bucket idx 30
    QCOMPARE(bucket_index(v.mid, 0.1), qint64(30));
    QCOMPARE(v.rows.size(), 3); // idx 31, 30, 29

    auto row29 = std::find_if(v.rows.begin(), v.rows.end(), [](const LadderRow& r) {
        return qAbs(r.price - bucket_price(2.94, 0.1)) < 1e-9;
    });
    QVERIFY(row29 != v.rows.end());
    QVERIFY(qAbs(row29->price - 2.9) < 1e-9);
    QCOMPARE(row29->bid_size, 3.0); // 1.0 + 2.0 summed, not 0
    QCOMPARE(row29->ask_size, 0.0);

    auto row30 = std::find_if(v.rows.begin(), v.rows.end(), [](const LadderRow& r) {
        return qAbs(r.price - bucket_price(3.02, 0.1)) < 1e-9;
    });
    QVERIFY(row30 != v.rows.end());
    QVERIFY(qAbs(row30->price - 3.0) < 1e-9);
    QCOMPARE(row30->ask_size, 4.0); // 3.0 + 1.0 summed, not 0
    QCOMPARE(row30->bid_size, 0.0);
}

QTEST_APPLESS_MAIN(TestCryptoLadderModel)
#include "tst_crypto_ladder_model.moc"
