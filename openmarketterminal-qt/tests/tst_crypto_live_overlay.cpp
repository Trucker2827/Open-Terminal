#include "screens/crypto_trading/CryptoLiveOverlay.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

namespace {
QJsonObject ord(const char* sym, const char* side, double px, double remaining, const char* status) {
    return QJsonObject{{"symbol", sym},    {"side", side},     {"price", px},
                       {"remaining", remaining}, {"status", status}, {"type", "limit"}};
}
} // namespace

class TstCryptoLiveOverlay : public QObject {
    Q_OBJECT
  private slots:
    void filters_symbol_status_and_priceless_orders() {
        const QVector<QJsonObject> in = {
            ord("BTC/USD", "buy", 118000, 0.001, "open"),
            ord("ETH/USD", "buy", 3000, 1.0, "open"),         // other symbol → dropped
            ord("BTC/USD", "sell", 119000, 0.002, "open"),
            ord("BTC/USD", "buy", 117000, 0.001, "canceled"), // non-open → dropped
            QJsonObject{{"symbol", "BTC/USD"}, {"side", "buy"}, {"price", 0.0},
                        {"remaining", 0.001},  {"status", "open"}, {"type", "market"}}, // no price → dropped
            ord("BTC/USD", "buy", 116000, 0.0, "open"),       // fully filled remainder → dropped
        };
        const auto out = live_orders_to_my_orders(in, QStringLiteral("BTC/USD"));
        QCOMPARE(out.size(), 2);
        QVERIFY(out[0].is_buy);
        QCOMPARE(out[0].price, 118000.0);
        QCOMPARE(out[0].qty, 0.001);
        QVERIFY(!out[1].is_buy);
        QCOMPARE(out[1].price, 119000.0);
    }
    void accepts_partially_filled() {
        const auto out = live_orders_to_my_orders({ord("BTC/USD", "buy", 118000, 0.5, "partially_filled")},
                                                  QStringLiteral("BTC/USD"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].qty, 0.5);
    }
    void vwap_avg_entry() {
        LiveAvgEntry a;
        a.add_trade(QStringLiteral("buy"), 100.0, 1.0);
        a.add_trade(QStringLiteral("buy"), 110.0, 1.0);
        QCOMPARE(a.net_qty(), 2.0);
        QCOMPARE(a.avg_entry(), 105.0);
        a.add_trade(QStringLiteral("sell"), 120.0, 1.0); // reduce → avg of remaining unchanged
        QCOMPARE(a.net_qty(), 1.0);
        QCOMPARE(a.avg_entry(), 105.0);
        a.add_trade(QStringLiteral("sell"), 120.0, 1.5); // net ≤ 0 → flat, no marker
        QVERIFY(a.net_qty() <= 0.0);
        QCOMPARE(a.avg_entry(), 0.0);
    }
    void ignores_nonpositive_trades() {
        LiveAvgEntry a;
        a.add_trade(QStringLiteral("buy"), 0.0, 1.0);
        a.add_trade(QStringLiteral("buy"), 100.0, 0.0);
        QCOMPARE(a.net_qty(), 0.0);
        QCOMPARE(a.avg_entry(), 0.0);
    }
    void reset_clears() {
        LiveAvgEntry a;
        a.add_trade(QStringLiteral("buy"), 100.0, 1.0);
        a.reset();
        QCOMPARE(a.net_qty(), 0.0);
        QCOMPARE(a.avg_entry(), 0.0);
    }
};

QTEST_MAIN(TstCryptoLiveOverlay)
#include "tst_crypto_live_overlay.moc"
