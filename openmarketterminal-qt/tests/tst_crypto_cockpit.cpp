#include "screens/crypto_trading/CryptoCockpitPresentation.h"

#include <QTimeZone>
#include <QtTest>

using openmarketterminal::screens::crypto::CryptoCockpitInputs;
using openmarketterminal::screens::crypto::CryptoCockpitSecurityInputs;
using openmarketterminal::screens::crypto::present_crypto_cockpit;
using openmarketterminal::screens::crypto::crypto_cockpit_security_blockers;
using openmarketterminal::screens::crypto::crypto_cockpit_qualification_of;
using openmarketterminal::screens::crypto::kCryptoQualificationFreshMs;

namespace {

QJsonObject decision(const QString& symbol, const QString& verdict, const QString& direction,
                     double net_bps, const QStringList& blockers = {}) {
    QJsonArray blocker_arr;
    for (const QString& b : blockers) blocker_arr.append(b);
    return QJsonObject{
        {QStringLiteral("symbol"), symbol},
        {QStringLiteral("verdict"), verdict},
        {QStringLiteral("direction"), direction},
        {QStringLiteral("reference_price"), 64000.0},
        {QStringLiteral("required_edge_bps"), 25.0},
        {QStringLiteral("net_after_cost_bps"), net_bps},
        {QStringLiteral("venue"), QStringLiteral("coinbase")},
        {QStringLiteral("liquidity"), QStringLiteral("taker")},
        {QStringLiteral("selected_proposal_index"), 0},
        {QStringLiteral("venue_proposals"),
         QJsonArray{QJsonObject{{QStringLiteral("venue"), QStringLiteral("kraken")},
                                {QStringLiteral("selected"), true}}}},
        {QStringLiteral("blockers"), blocker_arr}};
}

QJsonObject running_state(const QJsonArray& decisions) {
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("running")},
                       {QStringLiteral("heartbeat_at"),
                        QDateTime::fromMSecsSinceEpoch(1'000'000, QTimeZone::UTC)
                            .toString(Qt::ISODateWithMs)},
                       {QStringLiteral("config"),
                        QJsonObject{{QStringLiteral("enabled"), true},
                                    {QStringLiteral("style"), QStringLiteral("spot")}}},
                       {QStringLiteral("decisions"), decisions}};
}

} // namespace

class CryptoCockpitTest final : public QObject {
    Q_OBJECT

  private slots:
    void mood_is_paper_scalp_when_guard_disarmed() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.scalp_state = running_state({decision(QStringLiteral("BTC-USD"), QStringLiteral("WAIT"),
                                                     QStringLiteral("flat"), -1.0)});
        inputs.live_guard = QJsonObject{{QStringLiteral("enabled"), false}};
        inputs.security.cli_trading_allowed = true;
        inputs.security.cli_live_armed = true;
        inputs.security.cli_fast_live_armed = true;
        inputs.security.venue_allowed = true;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.mood, QStringLiteral("PAPER SCALP"));
        QVERIFY(!scene.canary_on);
        QVERIFY(!scene.mood.contains(QStringLiteral("LIVE ARMED")));
        QCOMPARE(scene.style, QStringLiteral("SPOT"));
        QCOMPARE(scene.engine_line, QStringLiteral("SPOT · RUNNING"));
    }

    void mood_is_canary_on_when_guard_armed_and_unexpired() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.scalp_state = running_state({});
        inputs.live_guard =
            QJsonObject{{QStringLiteral("enabled"), true},
                        {QStringLiteral("expires_at"),
                         QDateTime::fromMSecsSinceEpoch(2'000'000, QTimeZone::UTC)
                             .toString(Qt::ISODateWithMs)}};
        inputs.security.cli_trading_allowed = true;
        inputs.security.cli_live_armed = true;
        inputs.security.cli_fast_live_armed = true;
        inputs.security.venue_allowed = true;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.mood, QStringLiteral("CANARY ON"));
        QVERIFY(scene.canary_on);
        QVERIFY(scene.mood_detail.contains(QStringLiteral("expires")));
        QVERIFY(scene.blockers.isEmpty());
    }

    void expired_guard_is_not_canary_on() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 2'000'000;
        inputs.live_guard =
            QJsonObject{{QStringLiteral("enabled"), true},
                        {QStringLiteral("expires_at"),
                         QDateTime::fromMSecsSinceEpoch(1'000'000, QTimeZone::UTC)
                             .toString(Qt::ISODateWithMs)}};
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.mood, QStringLiteral("PAPER SCALP"));
        QVERIFY(!scene.canary_on);
    }

    void security_blockers_fail_closed() {
        CryptoCockpitSecurityInputs security;
        security.kill_switch = true;
        security.cli_trading_allowed = false;
        security.cli_live_armed = false;
        security.cli_fast_live_armed = false;
        security.venue_allowed = false;
        const QStringList blocked =
            crypto_cockpit_security_blockers(security, QStringLiteral("kraken"));
        QVERIFY(blocked.contains(QStringLiteral("kill switch is on")));
        QVERIFY(blocked.contains(QStringLiteral("CLI trading is off")));
        QVERIFY(blocked.contains(QStringLiteral("CLI LIVE trading is not armed")));
        QVERIFY(blocked.contains(QStringLiteral("FAST live mode is not armed")));
        QVERIFY(blocked.contains(QStringLiteral("kraken is not in allowed AI venues")));

        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.exchange_id = QStringLiteral("kraken");
        inputs.live_guard =
            QJsonObject{{QStringLiteral("enabled"), true},
                        {QStringLiteral("expires_at"),
                         QDateTime::fromMSecsSinceEpoch(2'000'000, QTimeZone::UTC)
                             .toString(Qt::ISODateWithMs)}};
        inputs.security = security;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.mood, QStringLiteral("CANARY ON"));
        QCOMPARE(scene.mood_role, QStringLiteral("amber"));
        QVERIFY(scene.mood_detail.contains(QStringLiteral("armed but blocked")));
        QCOMPARE(scene.blockers.size(), 5);
    }

    void qualification_mapping() {
        QString state, role, detail;
        crypto_cockpit_qualification_of({}, -1, &state, &role, &detail);
        QCOMPARE(state, QStringLiteral("UNAVAILABLE"));

        const QJsonObject ok{{QStringLiteral("report_version"), QStringLiteral("crypto-scalp-qualification-v1")},
                             {QStringLiteral("state"), QStringLiteral("QUALIFIED")},
                             {QStringLiteral("execution_eligible"), true}};
        crypto_cockpit_qualification_of(ok, 1'000, &state, &role, &detail);
        QCOMPARE(state, QStringLiteral("QUALIFIED"));
        QCOMPARE(role, QStringLiteral("green"));

        crypto_cockpit_qualification_of(ok, kCryptoQualificationFreshMs + 1, &state, &role, &detail);
        QCOMPARE(state, QStringLiteral("STALE"));
        QCOMPARE(role, QStringLiteral("amber"));

        const QJsonObject fail{{QStringLiteral("report_version"), QStringLiteral("crypto-scalp-qualification-v1")},
                               {QStringLiteral("state"), QStringLiteral("FAIL")},
                               {QStringLiteral("execution_eligible"), false}};
        crypto_cockpit_qualification_of(fail, 1'000, &state, &role, &detail);
        QCOMPARE(state, QStringLiteral("FAIL"));
        QCOMPARE(role, QStringLiteral("red"));
    }

    void tape_and_decide_come_from_state_decisions() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.scalp_state = running_state(
            {decision(QStringLiteral("BTC-USD"), QStringLiteral("BUY_CANDIDATE"),
                      QStringLiteral("up"), 12.5, {QStringLiteral("none")}),
             decision(QStringLiteral("ETH-USD"), QStringLiteral("WAIT"), QStringLiteral("flat"),
                      -3.0, {QStringLiteral("edge below hurdle")})});
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.decide_symbol, QStringLiteral("BTC-USD"));
        QVERIFY(scene.decide_is_candidate);
        QCOMPARE(scene.decide_liquidity, QStringLiteral("TAKER"));
        QCOMPARE(scene.decide_required, QStringLiteral("25.0 bps"));
        QCOMPARE(scene.tape.size(), 2);
        QCOMPARE(scene.tape.at(0).symbol, QStringLiteral("BTC-USD"));
        QCOMPARE(scene.tape.at(0).selected_venue, QStringLiteral("kraken"));
        QCOMPARE(scene.tape.at(1).symbol, QStringLiteral("ETH-USD"));
        QVERIFY(scene.tape_census.contains(QStringLiteral("2 decisions")));
        QCOMPARE(scene.decide_net, QStringLiteral("12.5 bps"));
        QVERIFY(!scene.mood.contains(QStringLiteral("LIVE ARMED")));
    }
};

QTEST_MAIN(CryptoCockpitTest)
#include "tst_crypto_cockpit.moc"
