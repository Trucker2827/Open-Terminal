// Known-answer coverage for the event-driven backtester (algo_engine/
// BacktestEngine::run) that powers the strategy backtest screen. Pure function:
// candles + strategy params -> flat metrics JSON. A silent error here reports
// fictitious returns/P&L to anyone evaluating a strategy.
//
// The fill model is fixed (no look-ahead): entry/exit SIGNALS latch on the
// close of bar i and fill at the OPEN of bar i+1; stop-loss / take-profit are
// checked intrabar against each bar's high/low. We drive deterministic single
// trades with a raw-price ("CLOSE") condition and assert exact P&L.

#include "algo_engine/BacktestEngine.h"
#include "algo_engine/AlgoEngineTypes.h"

#include <QtTest>

using openmarketterminal::algo::BacktestEngine;
using openmarketterminal::algo::OhlcvCandle;

namespace {

// A flat series of `n` daily bars, every OHLC == price.
QVector<OhlcvCandle> flatSeries(int n, double price) {
    QVector<OhlcvCandle> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        OhlcvCandle c;
        c.open_time = static_cast<int64_t>(i) * 86400000LL;  // 1 day apart
        c.close_time = c.open_time + 86400000LL;
        c.open = c.high = c.low = c.close = price;
        c.volume = 1000.0;
        c.is_closed = true;
        v.append(c);
    }
    return v;
}

// Entry condition: raw close price > `threshold`.
QJsonArray closeAbove(double threshold) {
    QJsonObject leaf;
    leaf["indicator"] = "CLOSE";
    leaf["operator"] = ">";
    leaf["value"] = threshold;
    return QJsonArray{leaf};
}

double D(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toDouble(); }

}  // namespace

class TestBacktestEngine : public QObject {
    Q_OBJECT
  private slots:
    void insufficientDataFails();
    void noTradesIsFlat();
    void takeProfitWinKnownPnl();
    void stopLossLossKnownPnl();
};

void TestBacktestEngine::insufficientDataFails() {
    // Needs >= kWarmupBars(50)+10 = 60 candles; 59 must fail closed.
    const QJsonObject out = BacktestEngine::run(flatSeries(59, 100.0), closeAbove(100.0), "AND",
                                                {}, "AND", 0, 0, 0, 10000.0, "1d");
    QCOMPARE(out.value("success").toBool(), false);
    QVERIFY(out.value("error").toString().contains("Insufficient", Qt::CaseInsensitive));
}

void TestBacktestEngine::noTradesIsFlat() {
    // Condition never triggers (close never exceeds 1e6) -> zero trades, capital
    // untouched, all performance metrics zero.
    const QJsonObject out = BacktestEngine::run(flatSeries(70, 100.0), closeAbove(1.0e6), "AND",
                                                {}, "AND", 0, 0, 0, 10000.0, "1d");
    QCOMPARE(out.value("success").toBool(), true);
    QCOMPARE(out.value("total_trades").toInt(), 0);
    QCOMPARE(D(out, "final_value"), 10000.0);
    QCOMPARE(D(out, "total_return_abs"), 0.0);
    QCOMPARE(D(out, "win_rate"), 0.0);
    QCOMPARE(D(out, "sharpe_ratio"), 0.0);
}

void TestBacktestEngine::takeProfitWinKnownPnl() {
    QVector<OhlcvCandle> c = flatSeries(70, 100.0);
    c[55].close = 101.0;       // close of bar 55 > 100 -> entry signal latched
    c[56].open = 101.0;        // fills here (entry price 101)
    c[56].high = 102.0;        // below tp (106.05) -> no same-bar exit
    c[57].high = 110.0;        // >= tp 106.05 -> take-profit fills at 106.05
    // bars 57+ close stay 100 (<= 100) so no re-entry.

    // take_profit 5% -> exit 106.05; shares floor(10000/101)=99; pnl 5.05*99=499.95.
    const QJsonObject out = BacktestEngine::run(c, closeAbove(100.0), "AND", {}, "AND",
                                                0.0, 5.0, 0.0, 10000.0, "1d");
    QCOMPARE(out.value("success").toBool(), true);
    QCOMPARE(out.value("total_trades").toInt(), 1);
    QCOMPARE(out.value("winning_trades").toInt(), 1);
    QCOMPARE(out.value("losing_trades").toInt(), 0);
    QCOMPARE(D(out, "win_rate"), 100.0);
    QCOMPARE(D(out, "final_value"), 10499.95);
    QCOMPARE(D(out, "total_return_abs"), 499.95);
    QCOMPARE(D(out, "profit_factor"), 999.99);  // no losses -> capped

    const QJsonObject t = out.value("trades").toArray().at(0).toObject();
    QCOMPARE(D(t, "entry_price"), 101.0);
    QCOMPARE(D(t, "exit_price"), 106.05);
    QCOMPARE(D(t, "shares"), 99.0);
    QCOMPARE(D(t, "pnl"), 499.95);
    QCOMPARE(t.value("reason").toString(), QStringLiteral("take_profit"));
}

void TestBacktestEngine::stopLossLossKnownPnl() {
    QVector<OhlcvCandle> c = flatSeries(70, 100.0);
    c[55].close = 101.0;       // entry signal
    c[56].open = 101.0;        // fills at 101
    c[56].low = 100.0;         // above stop (98.98) -> no same-bar exit
    c[57].low = 95.0;          // <= stop 98.98 -> stop fills at 98.98

    // stop_loss 2% -> exit 98.98; pnl (98.98-101)*99 = -199.98.
    const QJsonObject out = BacktestEngine::run(c, closeAbove(100.0), "AND", {}, "AND",
                                                2.0, 0.0, 0.0, 10000.0, "1d");
    QCOMPARE(out.value("total_trades").toInt(), 1);
    QCOMPARE(out.value("winning_trades").toInt(), 0);
    QCOMPARE(out.value("losing_trades").toInt(), 1);
    QCOMPARE(D(out, "win_rate"), 0.0);
    QCOMPARE(D(out, "final_value"), 9800.02);
    QCOMPARE(D(out, "total_return_abs"), -199.98);
    QCOMPARE(D(out, "profit_factor"), 0.0);  // no gross profit

    const QJsonObject t = out.value("trades").toArray().at(0).toObject();
    QCOMPARE(D(t, "exit_price"), 98.98);
    QCOMPARE(D(t, "pnl"), -199.98);
    QCOMPARE(t.value("reason").toString(), QStringLiteral("stop_loss"));
}

QTEST_APPLESS_MAIN(TestBacktestEngine)
#include "tst_backtest_engine.moc"
