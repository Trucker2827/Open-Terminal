#include "screens/crypto_trading/CryptoPaperFill.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;
using openmarketterminal::trading::OrderBookData;

namespace {
OrderBookData book2() { // asks: 100.0×1.0 then 101.0×2.0; bids: 99.0×1.0, 98.0×2.0
    OrderBookData b;
    b.symbol = "BTC/USD";
    b.asks = {{100.0, 1.0}, {101.0, 2.0}};
    b.bids = {{99.0, 1.0}, {98.0, 2.0}};
    b.best_bid = 99.0;
    b.best_ask = 100.0;
    return b;
}
} // namespace

class TstCryptoPaperFill : public QObject {
    Q_OBJECT
  private slots:
    void buy_walks_the_asks() {
        const auto v = paper_market_fill(QStringLiteral("buy"), 2.0, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.filled_qty, 2.0);
        QCOMPARE(v.fill_price, (100.0 * 1.0 + 101.0 * 1.0) / 2.0); // 100.5 size-weighted
        QCOMPARE(v.fee_paid, 2.0 * 100.5 * 0.004);
    }
    void sell_walks_the_bids() {
        const auto v = paper_market_fill(QStringLiteral("sell"), 1.5, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.fill_price, (99.0 * 1.0 + 98.0 * 0.5) / 1.5);
    }
    void partial_on_thin_book() {
        const auto v = paper_market_fill(QStringLiteral("buy"), 10.0, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.filled_qty, 3.0); // all visible ask depth
        QVERIFY(v.reason.contains(QStringLiteral("partial")));
    }
    void stale_book_rejects() {
        const auto v = paper_market_fill(QStringLiteral("buy"), 1.0, book2(), 5001, 0.004);
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains(QStringLiteral("stale")));
    }
    void empty_book_rejects() {
        const auto v = paper_market_fill(QStringLiteral("buy"), 1.0, OrderBookData{}, 100, 0.004);
        QVERIFY(!v.ok);
    }
    void bad_qty_rejects() {
        QVERIFY(!paper_market_fill(QStringLiteral("buy"), 0.0, book2(), 100, 0.004).ok);
        QVERIFY(!paper_market_fill(QStringLiteral("buy"), -1.0, book2(), 100, 0.004).ok);
        QVERIFY(!paper_market_fill(QStringLiteral("buy"),
                                   std::numeric_limits<double>::quiet_NaN(), book2(), 100, 0.004).ok);
    }
    void unknown_side_rejects() {
        QVERIFY(!paper_market_fill(QStringLiteral("hold"), 1.0, book2(), 100, 0.004).ok);
    }
};

QTEST_MAIN(TstCryptoPaperFill)
#include "tst_crypto_paper_fill.moc"
