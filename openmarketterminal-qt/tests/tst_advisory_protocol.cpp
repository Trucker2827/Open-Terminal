#include <QtTest>
#include "services/edge_radar/AdvisoryProtocol.h"
using namespace openmarketterminal;
class TstAdvisoryProtocol : public QObject {
    Q_OBJECT
private slots:
    void canonical_json_is_key_order_stable() {
        QJsonObject a{{"b",2},{"a",1}}; QJsonObject b{{"a",1},{"b",2}};
        QCOMPARE(adv::canonical_json(a), adv::canonical_json(b));
    }
    void canonical_json_is_nested_key_order_stable() {
        QJsonObject a{{"z", QJsonObject{{"b",2},{"a",1}}}, {"m", 3}};
        QJsonObject b{{"m", 3}, {"z", QJsonObject{{"a",1},{"b",2}}}};
        QCOMPARE(adv::canonical_json(a), adv::canonical_json(b));
    }
    void sha256_detects_mutation() {
        QJsonObject o{{"x",1}};
        const QString h1 = adv::sha256_hex(adv::canonical_json(o));
        o["x"] = 2;
        QVERIFY(adv::sha256_hex(adv::canonical_json(o)) != h1);
    }
    void ttl_is_horizon_aware() {
        QVERIFY(!adv::ttl_for(30).may_open);                       // <=60s: don't open
        QCOMPARE(adv::ttl_for(180).prediction_ttl_ms, qint64(15000));   // 1-5m -> 15s
        QCOMPARE(adv::ttl_for(3600 * 3).prediction_ttl_ms, qint64(60000)); // >60m -> 60s (== max)
        QVERIFY(adv::ttl_for(3600 * 3).execution_relevance_ms < adv::ttl_for(3600*3).prediction_ttl_ms);
    }
    void blind_packet_excludes_every_market_field() {
        QJsonObject snap{
            {"strike_floor", 60000}, {"seconds_left", 900}, {"spot", 61000},
            {"spot_microstructure", QJsonObject{{"aggressor_pressure", 0.2}}},
            {"market_implied_probability", 0.55}, {"fair_yes", 0.54},
            {"divergence", QJsonObject{{"label","DIVERGENCE"}}},
            {"daemon_probability", 0.55}, {"yes_ask", 0.56}};
        const QJsonObject blind = adv::build_blind_packet(snap);
        for (const QString& forbidden : adv::kBlindForbiddenKeys())
            QVERIFY2(!blind.contains(forbidden), qUtf8Printable("leaked: " + forbidden));
        QVERIFY(blind.contains("spot_microstructure"));   // allowlisted
        QVERIFY(blind.contains("strike_floor"));
    }
};
QTEST_MAIN(TstAdvisoryProtocol)
#include "tst_advisory_protocol.moc"
