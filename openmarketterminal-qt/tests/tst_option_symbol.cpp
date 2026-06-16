#include "trading/options/OptionSymbol.h"
#include <QtTest>

using namespace openmarketterminal::trading;

class TestOptionSymbol : public QObject {
    Q_OBJECT
  private slots:
    void occSymbolsAreOptions();
    void nonOptionsAreOne();
    void malformedIsOne();
};

void TestOptionSymbol::occSymbolsAreOptions() {
    for (const char* s : {"AAPL260821C00110000","SPY261218P00450000","F270115C00012500",
                          "TSLA260116P00250000","A260821C00050000"}) {
        QVERIFY2(is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 100);
    }
}
void TestOptionSymbol::nonOptionsAreOne() {
    for (const char* s : {"AAPL","MSFT","BRK.B","BTC/USD","NASDAQ:AAPL:265598","SPY","V"}) {
        QVERIFY2(!is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 1);
    }
}
void TestOptionSymbol::malformedIsOne() {
    for (const char* s : {"","AAPL26X","AAPL260821X00110000","AAPL26082C00110000",
                          "TOOLONGROOT260821C00110000","aapl260821c00110000"}) {
        QVERIFY2(!is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 1);
    }
}

QTEST_MAIN(TestOptionSymbol)
#include "tst_option_symbol.moc"
