#include <QtTest>
#include "services/prediction/kalshi/Kalshi15mReconcile.h"

namespace pred = openmarketterminal::services::prediction;

class TstKalshi15mReconcile : public QObject {
    Q_OBJECT
private slots:
    void desired_filters_by_family_and_caps() {
        QVector<pred::PredictionMarket> mk(3);
        mk[0].key.market_id = "KXBTC15M-26JUL270330-30";
        mk[1].key.market_id = "KXETH15M-26JUL270330-30";   // wrong family
        mk[2].key.market_id = "KXBTC15M-26JUL270345-45";
        const auto got = kalshi15m::desired_subscriptions(mk, {"KXBTC15M"}, 100);
        QCOMPARE(got, (QStringList{"KXBTC15M-26JUL270330-30",
                                   "KXBTC15M-26JUL270345-45"}));
    }
    void reconcile_computes_both_deltas() {
        const QStringList desired{"A", "B", "C"};
        const QSet<QString> held{"B", "C", "D"};
        const auto d = kalshi15m::reconcile(desired, held);
        QCOMPARE(d.to_subscribe, (QStringList{"A"}));
        QCOMPARE(d.to_unsubscribe, (QStringList{"D"}));
    }
    void reconcile_empty_delta_when_desired_equals_held() {
        // A ticker the controller never added (held only) must be unsubscribed
        // ONLY because it is 15m-held here; foreign UI tickers are never in held.
        const auto d = kalshi15m::reconcile({"A"}, {"A"});
        QVERIFY(d.to_subscribe.isEmpty());
        QVERIFY(d.to_unsubscribe.isEmpty());
    }
    void desired_truncates_to_cap() {
        QVector<pred::PredictionMarket> mk(3);
        mk[0].key.market_id = "KXBTC15M-26JUL270300-00";
        mk[1].key.market_id = "KXBTC15M-26JUL270315-15";
        mk[2].key.market_id = "KXBTC15M-26JUL270330-30";
        const auto got = kalshi15m::desired_subscriptions(mk, {"KXBTC15M"}, 2);
        QCOMPARE(got.size(), 2);
        QCOMPARE(got, (QStringList{"KXBTC15M-26JUL270300-00",
                                   "KXBTC15M-26JUL270315-15"}));
    }
};
QTEST_MAIN(TstKalshi15mReconcile)
#include "tst_kalshi_15m_reconcile.moc"
