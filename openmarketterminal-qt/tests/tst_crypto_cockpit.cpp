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
        QCOMPARE(scene.mood, QStringLiteral("PAPER AUTOMATION"));
        QVERIFY(!scene.canary_on);
        QVERIFY(!scene.mood.contains(QStringLiteral("LIVE ARMED")));
        QCOMPARE(scene.style, QStringLiteral("SPOT"));
        QCOMPARE(scene.engine_line, QStringLiteral("SPOT · RUNNING"));
        QCOMPARE(scene.spot_lane.state, QStringLiteral("ADAPTER PENDING"));
        QCOMPARE(scene.spot_lane.sample, QStringLiteral("0 / 100 FORWARD"));
        QCOMPARE(scene.scalp_lane.state, QStringLiteral("RUNNING"));
    }

    void independent_spot_lane_has_own_state_proof_and_ledger() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.spot_state = running_state(
            {decision(QStringLiteral("BTC-USD"), QStringLiteral("PAPER BUY CANDIDATE"),
                      QStringLiteral("up"), 18.5)});
        inputs.spot_state.insert(QStringLiteral("execution_authority"),
                                 QStringLiteral("paper_only"));
        inputs.spot_state.insert(QStringLiteral("decisions_path"),
                                 QStringLiteral("/daemon/btc1h_spot_decisions.jsonl"));
        inputs.spot_qualification = QJsonObject{
            {QStringLiteral("report_version"), QStringLiteral("crypto-spot-qualification-v1")},
            {QStringLiteral("state"), QStringLiteral("SHADOW")},
            {QStringLiteral("resolved_count"), 23},
            {QStringLiteral("required_resolved"), 100}};
        inputs.spot_qualification_age_ms = 1'000;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.spot_lane.state, QStringLiteral("RUNNING"));
        QCOMPARE(scene.spot_lane.authority, QStringLiteral("PAPER_ONLY"));
        QCOMPARE(scene.spot_lane.edge, QStringLiteral("18.5 bps"));
        QCOMPARE(scene.spot_lane.sample, QStringLiteral("23 / 100 FORWARD"));
        QVERIFY(scene.spot_lane.ledger.endsWith(QStringLiteral("btc1h_spot_decisions.jsonl")));
        QVERIFY(scene.spot_lane.detail.contains(QStringLiteral("inventory-backed")));
    }

    void active_symbol_never_leaks_btc_decision_into_eth_view() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.active_symbol = QStringLiteral("ETH/USD");
        inputs.scalp_state = running_state(
            {decision(QStringLiteral("BTC-USD"), QStringLiteral("PAPER TRADE CANDIDATE"),
                      QStringLiteral("up"), 12.0)});
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.scalp_lane.title, QStringLiteral("ETH SCALP SHADOW"));
        QCOMPARE(scene.scalp_lane.state, QStringLiteral("NOT TRACKED"));
        QCOMPARE(scene.scalp_lane.decision, QStringLiteral("NO DECISION YET"));
        QVERIFY(scene.decide_symbol.isEmpty());
        QVERIFY(scene.tape.isEmpty());
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
        QCOMPARE(scene.mood, QStringLiteral("PAPER AUTOMATION"));
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
        QCOMPARE(scene.tape.size(), 1);
        QCOMPARE(scene.tape.at(0).symbol, QStringLiteral("BTC-USD"));
        QCOMPARE(scene.tape.at(0).selected_venue, QStringLiteral("kraken"));
        QVERIFY(scene.tape_census.contains(QStringLiteral("1 of 2 decisions")));
        QCOMPARE(scene.decide_net, QStringLiteral("12.5 bps"));
        QVERIFY(!scene.mood.contains(QStringLiteral("LIVE ARMED")));
    }

    void shadow_proof_awaits_when_qualification_unresolved() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.scalp_state = running_state(
            {decision(QStringLiteral("BTC-USD"), QStringLiteral("WAIT"), QStringLiteral("flat"),
                      -1.0)});
        inputs.qualification = QJsonObject{
            {QStringLiteral("report_version"), QStringLiteral("crypto-scalp-qualification-v1")},
            {QStringLiteral("state"), QStringLiteral("SHADOW")},
            {QStringLiteral("execution_eligible"), false},
            {QStringLiteral("candidate_count"), 614},
            {QStringLiteral("resolved_count"), 0},
            {QStringLiteral("required_resolved"), 200},
            {QStringLiteral("coverage"), 0.0},
            {QStringLiteral("mean_net_bps"), 0.0},
            {QStringLiteral("mean_net_ci95"), QJsonArray{0.0, 0.0}},
            {QStringLiteral("win_rate"), 0.0},
            {QStringLiteral("resolved"), QJsonArray{}}};
        inputs.qualification_age_ms = 1'000;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.qualification_state, QStringLiteral("FAIL"));
        QVERIFY(scene.qualification_detail.contains(QStringLiteral("SHADOW")));
        QCOMPARE(scene.proof_all.verdict, QStringLiteral("AWAITING"));
        QCOMPARE(scene.proof_all.sample, QStringLiteral("0/614 · need 200"));
        QVERIFY(scene.proof_status.contains(QStringLiteral("scalp_decisions.jsonl")));
        QVERIFY(scene.proof_status.contains(QStringLiteral("Not edge crypto-recommend")));
        QVERIFY(!scene.proof_status.contains(QStringLiteral("SQLite sources")));
    }

    void shadow_proof_matches_qualified_report_metrics() {
        CryptoCockpitInputs inputs;
        inputs.now_ms = 1'000'000;
        inputs.scalp_state = running_state(
            {decision(QStringLiteral("BTC-USD"), QStringLiteral("PAPER TRADE CANDIDATE"),
                      QStringLiteral("up"), 8.0)});
        inputs.qualification = QJsonObject{
            {QStringLiteral("report_version"), QStringLiteral("crypto-scalp-qualification-v1")},
            {QStringLiteral("state"), QStringLiteral("QUALIFIED")},
            {QStringLiteral("execution_eligible"), true},
            {QStringLiteral("candidate_count"), 250},
            {QStringLiteral("resolved_count"), 210},
            {QStringLiteral("required_resolved"), 200},
            {QStringLiteral("coverage"), 0.84},
            {QStringLiteral("mean_net_bps"), 3.5},
            {QStringLiteral("mean_net_ci95"), QJsonArray{1.2, 5.8}},
            {QStringLiteral("win_rate"), 0.55},
            {QStringLiteral("resolved"),
             QJsonArray{QJsonObject{{QStringLiteral("symbol"), QStringLiteral("BTC-USD")},
                                    {QStringLiteral("net_bps"), 4.0},
                                    {QStringLiteral("won"), true}},
                        QJsonObject{{QStringLiteral("symbol"), QStringLiteral("BTC-USD")},
                                    {QStringLiteral("net_bps"), 2.0},
                                    {QStringLiteral("won"), false}},
                        QJsonObject{{QStringLiteral("symbol"), QStringLiteral("ETH-USD")},
                                    {QStringLiteral("net_bps"), -1.0},
                                    {QStringLiteral("won"), false}}}}};
        inputs.qualification_age_ms = 1'000;
        const auto scene = present_crypto_cockpit(inputs);
        QCOMPARE(scene.qualification_state, QStringLiteral("QUALIFIED"));
        QCOMPARE(scene.proof_all.verdict, QStringLiteral("QUALIFIED"));
        QCOMPARE(scene.proof_all.sample, QStringLiteral("210/250 · need 200"));
        QCOMPARE(scene.proof_all.mean_net, QStringLiteral("3.5 bps"));
        QCOMPARE(scene.proof_all.win_rate, QStringLiteral("55.0%"));
        QVERIFY(scene.proof_all.coverage.contains(QStringLiteral("84.0%")));
        QCOMPARE(scene.proof_symbol.scope, QStringLiteral("BTC-USD"));
        QCOMPARE(scene.proof_symbol.mean_net, QStringLiteral("3.0 bps"));
        QCOMPARE(scene.proof_symbol.win_rate, QStringLiteral("50.0%"));
        QVERIFY(scene.proof_status.contains(QStringLiteral("crypto-scalp-qualification-v1")));
        QVERIFY(scene.proof_status.contains(QStringLiteral("Not edge crypto-recommend")));
    }
};

QTEST_MAIN(CryptoCockpitTest)
#include "tst_crypto_cockpit.moc"
