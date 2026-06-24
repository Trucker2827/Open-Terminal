// Unit coverage for aggregate_candles() — the N→1 OHLCV bar aggregation used to
// synthesise timeframes Yahoo doesn't serve natively (3m from 1m, 4h from 60m).
// A silent bug here corrupts every backtest/feed that relies on a synthesised
// timeframe, so the math is pinned down here independently of the network fetch.

#include "algo_engine/CandleAggregation.h"

#include <QtTest>

using openmarketterminal::algo::aggregate_candles;
using openmarketterminal::algo::OhlcvCandle;

namespace {
OhlcvCandle bar(int64_t open_time, double o, double h, double l, double c, double v) {
    OhlcvCandle k;
    k.open_time = open_time;
    k.close_time = open_time + 60;  // arbitrary; overwritten by aggregation from the last bar
    k.open = o;
    k.high = h;
    k.low = l;
    k.close = c;
    k.volume = v;
    k.is_closed = true;
    return k;
}
}  // namespace

class TestCandleAggregation : public QObject {
    Q_OBJECT
  private slots:
    void factorOneIsIdentity();
    void factorZeroOrNegativeIsIdentity();
    void emptyInputIsEmpty();
    void aggregatesOhlcvCorrectly();
    void dropsIncompleteTrailingGroup();
};

void TestCandleAggregation::factorOneIsIdentity() {
    QVector<OhlcvCandle> in{bar(0, 1, 2, 0.5, 1.5, 10), bar(60, 1.5, 3, 1, 2, 20)};
    const auto out = aggregate_candles(in, 1);
    QCOMPARE(out.size(), in.size());
    QCOMPARE(out[0].open, in[0].open);
    QCOMPARE(out[1].close, in[1].close);
}

void TestCandleAggregation::factorZeroOrNegativeIsIdentity() {
    QVector<OhlcvCandle> in{bar(0, 1, 2, 0.5, 1.5, 10)};
    QCOMPARE(aggregate_candles(in, 0).size(), in.size());
    QCOMPARE(aggregate_candles(in, -5).size(), in.size());
}

void TestCandleAggregation::emptyInputIsEmpty() {
    QCOMPARE(aggregate_candles({}, 3).size(), 0);
}

void TestCandleAggregation::aggregatesOhlcvCorrectly() {
    // Three 1m bars → one 3m bar.
    QVector<OhlcvCandle> in{
        bar(0,   10.0, 12.0, 9.0,  11.0, 100),   // open of the group
        bar(60,  11.0, 15.0, 8.0,  14.0, 200),   // contains the high (15) and low (8)
        bar(120, 14.0, 14.5, 13.0, 13.5, 300),   // close of the group
    };
    const auto out = aggregate_candles(in, 3);
    QCOMPARE(out.size(), 1);
    const OhlcvCandle& c = out[0];
    QCOMPARE(c.open, 10.0);            // first bar's open
    QCOMPARE(c.open_time, int64_t(0)); // first bar's open_time (copied through)
    QCOMPARE(c.high, 15.0);            // max across the group
    QCOMPARE(c.low, 8.0);              // min across the group
    QCOMPARE(c.close, 13.5);           // last bar's close
    QCOMPARE(c.close_time, int64_t(180));  // last bar's close_time (120 + 60)
    QCOMPARE(c.volume, 600.0);         // sum of volumes
    QVERIFY(c.is_closed);
}

void TestCandleAggregation::dropsIncompleteTrailingGroup() {
    // Seven 1m bars, factor 3 → two complete 3m bars; the 7th bar is dropped so
    // we never emit a partial/misleading bar.
    QVector<OhlcvCandle> in;
    for (int i = 0; i < 7; ++i)
        in.append(bar(i * 60, 1.0 + i, 2.0 + i, i, 1.5 + i, 10));
    const auto out = aggregate_candles(in, 3);
    QCOMPARE(out.size(), 2);
    // Group 1 = bars [0,1,2]; group 2 = bars [3,4,5]; bar 6 dropped.
    QCOMPARE(out[0].open, in[0].open);
    QCOMPARE(out[0].close, in[2].close);
    QCOMPARE(out[1].open, in[3].open);
    QCOMPARE(out[1].close, in[5].close);
    QCOMPARE(out[0].volume, 30.0);
    QCOMPARE(out[1].volume, 30.0);
}

QTEST_APPLESS_MAIN(TestCandleAggregation)
#include "tst_candle_aggregation.moc"
