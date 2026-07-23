#include "services/crypto_scalp/CryptoAutoScalp.h"

#include <QtTest>

using namespace openmarketterminal::services;
using namespace openmarketterminal::services::crypto_scalp;

namespace {
ClosedBar bar(qint64 ts, double open, double high, double low, double close) {
    return ClosedBar{ts, open, high, low, close, 4};
}

crypto_latency::CryptoLatencyTick tick(qint64 ts, double px) {
    crypto_latency::CryptoLatencyTick t;
    t.source = QStringLiteral("coinbase");
    t.received_ts_ms = ts;
    t.exchange_ts_ms = ts;
    t.price = px;
    t.best_bid = px - 0.5;
    t.best_ask = px + 0.5;
    return t;
}
}

class CryptoAutoScalpTest final : public QObject {
    Q_OBJECT

  private slots:
    void forming_bucket_can_never_repaint_closed_bars() {
        QVector<crypto_latency::CryptoLatencyTick> ticks{
            tick(1000, 100), tick(1200, 101), tick(2000, 102), tick(2200, 103),
            tick(3000, 104)};
        const auto before = build_closed_bars(ticks, 1000);
        QCOMPARE(before.size(), 2);
        QCOMPARE(before.last().close, 103.0);

        // Extreme prices inside the current 3-second bucket cannot change any
        // closed bar or create a signal early.
        ticks << tick(3100, 50) << tick(3200, 200);
        const auto after = build_closed_bars(ticks, 1000);
        QCOMPARE(after.size(), before.size());
        QCOMPARE(after.last().close, before.last().close);
        QCOMPARE(after.last().high, before.last().high);
        QCOMPARE(after.last().low, before.last().low);
    }

    void closed_structure_break_emits_buy_and_sell() {
        QVector<ClosedBar> up{
            bar(1000,100,101,99,100), bar(2000,100,103,100,102),
            bar(3000,102,102,98,99),  bar(4000,99,101,97,100),
            bar(5000,100,102,99,101), bar(6000,101,101.5,99,100),
            bar(7000,100,102,99.5,101), bar(8000,101,102.5,100,102),
            bar(9000,102,104,101.5,103.5)};
        const auto buy = evaluate_reversal(up, 1, 1, 4);
        QCOMPARE(buy.call, QStringLiteral("BUY"));
        QVERIFY(buy.expected_move_bps > 0.0);

        QVector<ClosedBar> down{
            bar(1000,103,104,102,103), bar(2000,103,104,100,101),
            bar(3000,101,105,101,104), bar(4000,104,104.5,102,103),
            bar(5000,103,104,102,103), bar(6000,103,104,102.5,103),
            bar(7000,103,103.5,101.5,102), bar(8000,102,102.5,100.5,101),
            bar(9000,101,101.5,98,98.5)};
        const auto sell = evaluate_reversal(down, 1, 1, 4);
        QCOMPARE(sell.call, QStringLiteral("SELL"));
        QVERIFY(sell.expected_move_bps > 0.0);
    }

    void both_venues_are_costed_and_router_selects_net_edge() {
        ReversalSignal signal;
        signal.call = QStringLiteral("BUY");
        signal.expected_move_bps = 70.0;
        crypto_latency::CryptoLatencyTick coinbase = tick(10'000, 100.0);
        coinbase.source = QStringLiteral("coinbase");
        coinbase.best_bid = 99.99;
        coinbase.best_ask = 100.01;
        crypto_latency::CryptoLatencyTick kraken = tick(10'000, 100.0);
        kraken.source = QStringLiteral("kraken");
        kraken.best_bid = 99.99;
        kraken.best_ask = 100.01;
        const VenueCost cb{QStringLiteral("coinbase_advanced"), QStringLiteral("coinbase"),
                           10.0, 20.0, 1.0, 2.0};
        const VenueCost kr{QStringLiteral("kraken_pro"), QStringLiteral("kraken"),
                           5.0, 12.0, 1.0, 2.0};
        QVector<VenueProposal> proposals{
            build_proposal(signal, coinbase, cb, QStringLiteral("taker"),
                           10'010, 250, 5.0, 750),
            build_proposal(signal, kraken, kr, QStringLiteral("taker"),
                           10'010, 250, 5.0, 750)};
        QCOMPARE(proposals.size(), 2);
        QVERIFY(proposals[0].executable);
        QVERIFY(proposals[1].executable);
        QCOMPARE(choose_best_proposal(proposals), 1);
        QCOMPARE(proposals[1].time_in_force, QStringLiteral("IOC"));
    }

    void stale_or_fee_dead_proposals_fail_closed() {
        ReversalSignal signal;
        signal.call = QStringLiteral("SELL");
        signal.expected_move_bps = 5.0;
        auto quote = tick(1000, 100.0);
        const VenueCost cost{QStringLiteral("kraken_pro"), QStringLiteral("kraken"),
                             20.0, 40.0, 2.0, 5.0};
        const auto proposal = build_proposal(signal, quote, cost, QStringLiteral("taker"),
                                             5000, 250, 1.0, 750);
        QVERIFY(!proposal.executable);
        QVERIFY(proposal.blockers.contains(QStringLiteral("STALE_QUOTE")));
        QVERIFY(proposal.blockers.contains(QStringLiteral("COST_NET_EDGE_BELOW_GATE")));
    }
};

QTEST_GUILESS_MAIN(CryptoAutoScalpTest)
#include "tst_crypto_auto_scalp.moc"
