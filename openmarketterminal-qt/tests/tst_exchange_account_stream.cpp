#include "trading/AccountStreamParse.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::trading;

class TstExchangeAccountStream : public QObject {
    Q_OBJECT
  private slots:
    void routes_order() {
        const QJsonObject line{
            {"type", "account_order"},
            {"order", QJsonObject{{"id", "o1"}, {"symbol", "BTC/USD"}, {"side", "buy"}, {"status", "open"}}}};
        const AccountLine r = parse_account_line(line);
        QCOMPARE(r.kind, QString("order"));
        QCOMPARE(r.symbol, QString("BTC/USD"));
        QCOMPARE(r.payload.value("id").toString(), QString("o1"));
    }
    void routes_mytrade() {
        const QJsonObject line{
            {"type", "account_mytrade"},
            {"trade", QJsonObject{{"id", "t1"}, {"order", "o1"}, {"symbol", "ETH/USD"}, {"price", 3000.0}}}};
        const AccountLine r = parse_account_line(line);
        QCOMPARE(r.kind, QString("mytrade"));
        QCOMPARE(r.symbol, QString("ETH/USD"));
        QCOMPARE(r.payload.value("order").toString(), QString("o1"));
    }
    void routes_balance() {
        const QJsonObject line{
            {"type", "account_balance"},
            {"balances", QJsonObject{{"USD", QJsonObject{{"free", 100.0}, {"total", 100.0}}}}}};
        const AccountLine r = parse_account_line(line);
        QCOMPARE(r.kind, QString("balance"));
        QVERIFY(r.symbol.isEmpty());
        QVERIFY(r.payload.contains("USD"));
    }
    void public_lines_untouched() {
        for (const char* t : {"ticker", "orderbook", "ohlcv", "trade", "status", "error", "symbol_map"}) {
            const AccountLine r = parse_account_line(QJsonObject{{"type", t}});
            QVERIFY2(r.kind.isEmpty(), t);
        }
    }
};

QTEST_MAIN(TstExchangeAccountStream)
#include "tst_exchange_account_stream.moc"
