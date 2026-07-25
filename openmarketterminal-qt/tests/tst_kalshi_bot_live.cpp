// Micro-live containment for the Kalshi bot (ladder rung 5, issue #130).
//
// Every one of the charter carve-out's conditions gets a test that breaks
// ONLY that condition, starting from a baseline that is permitted — so a green
// suite proves each refusal is bound to its own condition rather than passing
// because something else refused first.
//
// The tests that matter most here are the ones about what a live row is NOT
// allowed to say: no paper fill model on a live order, no live outcome inside
// the paper promotion gate, and no "submitted" on an order the venue refused.

#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotGate.h"
#include "services/prediction/kalshi/KalshiBotLive.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <cmath>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotGate;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotLive;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotOrders;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotStopFile;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

/// A `kalshi auto live status` object for a bounded, human-armed session with
/// every revocable gate open — the only shape in which live bidding is allowed.
QJsonObject armed_status() {
    return QJsonObject{
        {QStringLiteral("session_active"), true},
        {QStringLiteral("live_armed"), true},
        {QStringLiteral("trading_allowed"), true},
        {QStringLiteral("venue_allowed"), true},
        {QStringLiteral("kill_switch"), false},
        {QStringLiteral("per_bet_contract_stake_cap"), 2.0},
        {QStringLiteral("per_bet_all_in_tolerance"), 3.0},
        {QStringLiteral("experiment_cap"), 120.0},
        {QStringLiteral("max_orders_per_hour"), 10},
        {QStringLiteral("session"),
         QJsonObject{{QStringLiteral("session_id"), QStringLiteral("sess-1")},
                     {QStringLiteral("ends_at"), iso(kNow + 3'600'000)}}}};
}

/// A PASS verdict published minutes ago, in KalshiBotGate::evaluate()'s shape.
QJsonObject passing_gate(qint64 ts_ms = kNow - 60'000) {
    return QJsonObject{{QStringLiteral("verdict"), QStringLiteral("PASS")},
                       {QStringLiteral("evaluated"), true},
                       {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)}};
}

/// One bid row exactly as KalshiBotDecision::decide() writes it.
QJsonObject bid_row() {
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                       {QStringLiteral("ts_ms"), static_cast<double>(kNow)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("live_eligible"), false},
                       {QStringLiteral("ticker"), QStringLiteral("KXBTC15M-T1")},
                       {QStringLiteral("action"), QStringLiteral("bid")},
                       {QStringLiteral("reason_code"), QStringLiteral("EDGE_CLEARS_THRESHOLD")},
                       {QStringLiteral("side"), QStringLiteral("YES")},
                       {QStringLiteral("price"), 0.40},
                       {QStringLiteral("contracts"), 5},
                       {QStringLiteral("stake_usd"), 2.00},
                       {QStringLiteral("fee_usd"), 0.06},
                       {QStringLiteral("all_in_usd"), 2.06},
                       {QStringLiteral("calibrated_p"), 0.62},
                       {QStringLiteral("market_mid"), 0.40},
                       {QStringLiteral("position_id"), QStringLiteral("KXBTC15M-T1@1")},
                       {QStringLiteral("order_state"), QStringLiteral("resting")},
                       {QStringLiteral("limit_price"), 0.40},
                       {QStringLiteral("filled_count"), 0},
                       {QStringLiteral("remaining_count"), 5},
                       {QStringLiteral("ttl_ms"), 180'000.0},
                       {QStringLiteral("expires_at_ms"), static_cast<double>(kNow + 180'000)},
                       {QStringLiteral("exposure_after_usd"), 2.00},
                       {QStringLiteral("exposure_cap_usd"), 120.0}};
}

KalshiBotLive::Permission permitted() {
    return KalshiBotLive::permit(armed_status(), passing_gate(), {}, kNow);
}

} // namespace

class TstKalshiBotLive : public QObject {
    Q_OBJECT

  private slots:
    // ── the baseline: every carve-out condition true at once ────────────────

    void permits_only_when_every_condition_holds() {
        const KalshiBotLive::Permission permission = permitted();
        QVERIFY2(permission.permitted, qPrintable(permission.reason_code + ": " + permission.detail));
        QVERIFY(permission.reason_code.isEmpty());
        QCOMPARE(permission.max_stake_usd, 2.0);
        QCOMPARE(permission.max_all_in_usd, 3.0);
        QCOMPARE(permission.experiment_cap_usd, 120.0);
        QCOMPARE(permission.max_orders_per_hour, 10);
        QCOMPARE(permission.session_id, QStringLiteral("sess-1"));
    }

    // ── condition 1: a human armed the session ──────────────────────────────

    void kill_switch_refuses_before_anything_else() {
        KalshiBotStopFile stop;
        stop.engaged = true;
        // Everything else is perfect; only the switch is thrown.
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(armed_status(), passing_gate(), stop, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedStopped));
    }

    void unreadable_status_is_neither_armed_nor_disarmed() {
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(QJsonObject{}, passing_gate(), {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedStatusUnknown));
    }

    void each_arm_flag_alone_refuses_data() {
        QTest::addColumn<QString>("flag");
        QTest::addColumn<bool>("value");
        QTest::newRow("session_active") << QStringLiteral("session_active") << false;
        QTest::newRow("live_armed") << QStringLiteral("live_armed") << false;
        QTest::newRow("trading_allowed") << QStringLiteral("trading_allowed") << false;
        QTest::newRow("venue_allowed") << QStringLiteral("venue_allowed") << false;
        QTest::newRow("kill_switch") << QStringLiteral("kill_switch") << true;
    }

    void each_arm_flag_alone_refuses() {
        QFETCH(QString, flag);
        QFETCH(bool, value);
        QJsonObject status = armed_status();
        status.insert(flag, value);
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY2(!permission.permitted, qPrintable(flag + " alone must refuse live mode"));
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedNotArmed));
    }

    void a_session_without_an_id_is_not_an_armed_session() {
        QJsonObject status = armed_status();
        status.insert(QStringLiteral("session"),
                      QJsonObject{{QStringLiteral("ends_at"), iso(kNow + 3'600'000)}});
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedNotArmed));
    }

    // ── condition 1b: the run is BOUNDED ────────────────────────────────────

    // The trap this test exists for: `kalshi auto live session 24/7` writes an
    // EMPTY ends_at, and the terminal's own activity test reads a missing end
    // as "not expired", so the session is fully ACTIVE and fully ARMED. Only a
    // separate boundedness check catches it.
    void an_unbounded_24_7_arm_is_refused_even_though_it_is_active() {
        QJsonObject status = armed_status();
        status.insert(QStringLiteral("session"),
                      QJsonObject{{QStringLiteral("session_id"), QStringLiteral("sess-247")},
                                  {QStringLiteral("ends_at"), QString()}});
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY2(!permission.permitted, "a 24/7 arm reads active — it must still be refused");
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedUnbounded));
    }

    void an_already_expired_end_is_refused() {
        QJsonObject status = armed_status();
        status.insert(QStringLiteral("session"),
                      QJsonObject{{QStringLiteral("session_id"), QStringLiteral("sess-old")},
                                  {QStringLiteral("ends_at"), iso(kNow - 1)}});
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedUnbounded));
    }

    // ── condition 3: the preregistered gate reads a fresh PASS ──────────────

    void no_gate_file_refuses() {
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(armed_status(), QJsonObject{}, {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedGateMissing));
    }

    void a_gate_refusal_is_not_a_pass() {
        // TAMPERED / NOT_PREREGISTERED verdicts carry no criteria at all.
        const KalshiBotLive::Permission permission = KalshiBotLive::permit(
            armed_status(),
            QJsonObject{{QStringLiteral("verdict"), QString::fromLatin1(KalshiBotGate::kVerdictTampered)},
                        {QStringLiteral("evaluated"), false},
                        {QStringLiteral("reason"), QStringLiteral("seal check failed")},
                        {QStringLiteral("ts_ms"), static_cast<double>(kNow)}},
            {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedGateRefused));
    }

    void a_fail_verdict_refuses() {
        QJsonObject gate = passing_gate();
        gate.insert(QStringLiteral("verdict"), QString::fromLatin1(KalshiBotGate::kVerdictFail));
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(armed_status(), gate, {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedGateFail));
    }

    void a_stale_pass_refuses() {
        const KalshiBotLive::Permission permission = KalshiBotLive::permit(
            armed_status(), passing_gate(kNow - KalshiBotLive::kMaxGateAgeMs - 1), {}, kNow);
        QVERIFY2(!permission.permitted, "a PASS older than the freshness bound is a stale opinion");
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedGateStale));
    }

    void an_undated_or_future_pass_refuses() {
        QJsonObject undated = passing_gate();
        undated.remove(QStringLiteral("ts_ms"));
        QCOMPARE(KalshiBotLive::permit(armed_status(), undated, {}, kNow).reason_code,
                 QString::fromLatin1(KalshiBotLive::kRefusedGateStale));
        QCOMPARE(KalshiBotLive::permit(armed_status(), passing_gate(kNow + 1), {}, kNow).reason_code,
                 QString::fromLatin1(KalshiBotLive::kRefusedGateStale));
    }

    // ── condition 2: the caps are the session's, and only tighten ───────────

    void the_intent_carries_the_armed_sessions_caps_verbatim() {
        const KalshiBotLive::Permission permission = permitted();
        const QJsonObject intent = KalshiBotLive::live_intent(bid_row(), permission);
        QVERIFY(!intent.isEmpty());
        QCOMPARE(intent.value(QStringLiteral("max_live_stake")).toDouble(), 2.0);
        QCOMPARE(intent.value(QStringLiteral("max_live_all_in")).toDouble(), 3.0);
        QCOMPARE(intent.value(QStringLiteral("experiment_loss_cap")).toDouble(), 120.0);
        QCOMPARE(intent.value(QStringLiteral("max_orders_per_hour")).toInt(), 10);
        QCOMPARE(intent.value(QStringLiteral("automation_session_id")).toString(),
                 QStringLiteral("sess-1"));
        // The two tags without which submit_order does not run the micro-live
        // gate stack at all.
        QCOMPARE(intent.value(QStringLiteral("experiment_id")).toString(),
                 QString::fromLatin1(KalshiBotLive::kExperimentId));
        QVERIFY(intent.value(QStringLiteral("autonomous")).toBool());
        // Fill-and-kill: this rung has no live cancel path, so nothing may be
        // left resting unattended at the venue.
        QCOMPARE(intent.value(QStringLiteral("order_type")).toString(), QStringLiteral("fak"));
        QCOMPARE(intent.value(QStringLiteral("estimated_fee")).toDouble(), 0.06);
        QCOMPARE(intent.value(QStringLiteral("limit_price")).toDouble(), 0.40);
        QCOMPARE(intent.value(QStringLiteral("contracts")).toInt(), 5);
        QCOMPARE(intent.value(QStringLiteral("outcome")).toString(), QStringLiteral("Yes"));
        QCOMPARE(intent.value(QStringLiteral("side")).toString(), QStringLiteral("buy"));
    }

    // A session file that somehow states a cap ABOVE the charter's ceiling
    // cannot buy the bot more authority: what it asks for is clamped down.
    void a_session_cap_above_the_charter_ceiling_is_clamped_not_honoured() {
        QJsonObject status = armed_status();
        status.insert(QStringLiteral("per_bet_contract_stake_cap"), 50.0);
        status.insert(QStringLiteral("per_bet_all_in_tolerance"), 75.0);
        status.insert(QStringLiteral("experiment_cap"), 5000.0);
        status.insert(QStringLiteral("max_orders_per_hour"), 900);
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY(permission.permitted);
        QCOMPARE(permission.max_stake_usd, 2.0);
        QCOMPARE(permission.max_all_in_usd, 3.0);
        QCOMPARE(permission.experiment_cap_usd, 120.0);
        QCOMPARE(permission.max_orders_per_hour, 10);
    }

    void a_status_without_caps_refuses() {
        QJsonObject status = armed_status();
        status.remove(QStringLiteral("per_bet_all_in_tolerance"));
        const KalshiBotLive::Permission permission =
            KalshiBotLive::permit(status, passing_gate(), {}, kNow);
        QVERIFY(!permission.permitted);
        QCOMPARE(permission.reason_code, QString::fromLatin1(KalshiBotLive::kRefusedStatusUnknown));
    }

    void a_refused_permission_builds_no_intent() {
        KalshiBotLive::Permission refused;
        QVERIFY(KalshiBotLive::live_intent(bid_row(), refused).isEmpty());
    }

    void a_malformed_bid_row_builds_no_intent() {
        const KalshiBotLive::Permission permission = permitted();
        QJsonObject row = bid_row();
        row.remove(QStringLiteral("fee_usd"));
        QVERIFY2(KalshiBotLive::live_intent(row, permission).isEmpty(),
                 "a missing fee fails closed at submit — guessing one here would turn a clean "
                 "refusal into a rejection");
        row = bid_row();
        row.insert(QStringLiteral("action"), QStringLiteral("pass"));
        QVERIFY(KalshiBotLive::live_intent(row, permission).isEmpty());
        row = bid_row();
        row.insert(QStringLiteral("contracts"), 0);
        QVERIFY(KalshiBotLive::live_intent(row, permission).isEmpty());
    }

    // ── the live row says only what the venue said ──────────────────────────

    void an_accepted_order_is_journaled_live_with_the_venues_own_state() {
        const QJsonObject row = KalshiBotLive::live_row(
            bid_row(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("partially_filled")},
                        {QStringLiteral("filled_count"), 2.0},
                        {QStringLiteral("remaining_count"), 3.0}},
            permitted());
        QCOMPARE(row.value(QStringLiteral("mode")).toString(), QStringLiteral("live"));
        QVERIFY(row.value(QStringLiteral("live_eligible")).toBool());
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotLive::kLiveSubmitted));
        QCOMPARE(row.value(QStringLiteral("order_state")).toString(),
                 QStringLiteral("partially_filled"));
        QCOMPARE(row.value(QStringLiteral("filled_count")).toDouble(), 2.0);
        QCOMPARE(row.value(QStringLiteral("remaining_count")).toDouble(), 3.0);
        // Why the bid was made is kept; it is just no longer the row's headline.
        QCOMPARE(row.value(QStringLiteral("decision_reason_code")).toString(),
                 QStringLiteral("EDGE_CLEARS_THRESHOLD"));
        // The armed session's caps travel with the row, so the ledger can be
        // audited without the session file that produced it.
        QCOMPARE(row.value(QStringLiteral("live_caps")).toObject()
                     .value(QStringLiteral("session_id")).toString(),
                 QStringLiteral("sess-1"));
    }

    void a_rejected_order_is_never_journaled_as_placed() {
        const QJsonObject row = KalshiBotLive::live_row(
            bid_row(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("rejected")},
                        {QStringLiteral("reason"), QStringLiteral("micro-live all-in cap exceeded")}},
            permitted());
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotLive::kLiveRejected));
        QVERIFY2(!row.contains(QStringLiteral("order_state")),
                 "a refused order has no state at the venue");
        QCOMPARE(row.value(QStringLiteral("submit_reason")).toString(),
                 QStringLiteral("micro-live all-in cap exceeded"));
    }

    // An empty status is what a failed tool call leaves behind. Reading it as
    // anything but "not placed" would book an order nobody can prove exists.
    void a_silent_submit_path_is_read_as_rejected() {
        const QJsonObject row = KalshiBotLive::live_row(bid_row(), QJsonObject{}, permitted());
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotLive::kLiveRejected));
        QCOMPARE(row.value(QStringLiteral("submit_status")).toString(),
                 QStringLiteral("(no status returned)"));
        QVERIFY(!row.contains(QStringLiteral("order_state")));
    }

    // The paper fill model never described a live order and must not appear to.
    void a_live_row_carries_no_paper_fill_model_fields() {
        const QJsonObject row = KalshiBotLive::live_row(
            bid_row(), QJsonObject{{QStringLiteral("status"), QStringLiteral("filled")}},
            permitted());
        for (const char* key : {"ttl_ms", "expires_at_ms", "exposure_after_usd", "exposure_cap_usd",
                                "fill_model", "fill_rule"})
            QVERIFY2(!row.contains(QLatin1String(key)),
                     qPrintable(QStringLiteral("a live row must not carry the paper field '%1'")
                                    .arg(QLatin1String(key))));
    }

    void a_refusal_row_carries_no_trading_numbers() {
        KalshiBotLive::Permission refused;
        refused.reason_code = QString::fromLatin1(KalshiBotLive::kRefusedGateFail);
        refused.detail = QStringLiteral("the gate reads FAIL");
        const QJsonObject row = KalshiBotLive::refusal_row(refused, kNow);
        QCOMPARE(row.value(QStringLiteral("mode")).toString(), QStringLiteral("live"));
        QVERIFY2(!row.value(QStringLiteral("live_eligible")).toBool(),
                 "a refused tick was not live-eligible");
        for (const char* key : {"ticker", "price", "contracts", "stake_usd", "all_in_usd",
                                "max_stake_usd", "live_caps"})
            QVERIFY2(!row.contains(QLatin1String(key)),
                     qPrintable(QStringLiteral("a refusal must not state '%1'")
                                    .arg(QLatin1String(key))));
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotLive::kRefusedGateFail));
    }

    // ── quarantine: live rows never touch the paper machinery ───────────────

    // KalshiBotOrders replays the PAPER book with a fill model that infers a
    // fill from an observed mid. Running it over a live order would invent a
    // fill the exchange never gave.
    void the_paper_book_never_replays_a_live_order() {
        const QJsonObject paper = bid_row();
        QJsonObject live = bid_row();
        live.insert(QStringLiteral("mode"), QStringLiteral("live"));
        live.insert(QStringLiteral("ticker"), QStringLiteral("KXBTC15M-T2"));
        live.insert(QStringLiteral("position_id"), QStringLiteral("KXBTC15M-T2@1"));

        const KalshiBotOrders::Book both = KalshiBotOrders::replay(QJsonArray{paper, live});
        QCOMPARE(both.resting.size(), 1);
        QCOMPARE(both.resting.first().toObject().value(QStringLiteral("ticker")).toString(),
                 QStringLiteral("KXBTC15M-T1"));
        const KalshiBotOrders::Book paper_only = KalshiBotOrders::replay(QJsonArray{paper});
        QCOMPARE(both.exposure_usd, paper_only.exposure_usd);
    }

    // Carve-out condition 3 requires the gate to have passed on PAPER results.
    // A live outcome scored here would be the bot grading itself on the trades
    // its own promotion authorised.
    void the_promotion_gate_never_scores_a_live_outcome() {
        QJsonObject params{{QStringLiteral("schema"), 1},
                           {QStringLiteral("event"), QStringLiteral("kalshi_bot_gate_params")},
                           {QStringLiteral("gate_id"), QStringLiteral("test-gate")},
                           {QStringLiteral("sealed_at_ms"), static_cast<double>(kNow - 86'400'000)},
                           {QStringLiteral("params"),
                            QJsonObject{{QStringLiteral("min_settled_bids"), 300},
                                        {QStringLiteral("max_drawdown_usd"), 50.0}}}};
        params.insert(QStringLiteral("seal_sha256"), KalshiBotGate::seal(params));

        const auto settled_pair = [](const QString& mode, int index, double pnl) {
            const QString id = QStringLiteral("T%1@%2").arg(index).arg(index);
            QJsonObject decision{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                                 {QStringLiteral("action"), QStringLiteral("bid")},
                                 {QStringLiteral("position_id"), id},
                                 {QStringLiteral("ts_ms"), static_cast<double>(kNow + index)},
                                 {QStringLiteral("calibrated_p"), 0.9},
                                 {QStringLiteral("market_mid"), 0.5},
                                 {QStringLiteral("side"), QStringLiteral("YES")}};
            QJsonObject settlement{
                {QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
                {QStringLiteral("position_id"), id},
                {QStringLiteral("ts_ms"), static_cast<double>(kNow + index)},
                {QStringLiteral("realized_pnl"), pnl},
                {QStringLiteral("market_result"), QStringLiteral("YES")}};
            if (!mode.isEmpty()) {
                decision.insert(QStringLiteral("mode"), mode);
                settlement.insert(QStringLiteral("mode"), mode);
            }
            return std::pair<QJsonObject, QJsonObject>{decision, settlement};
        };

        QJsonArray decisions;
        QJsonArray settlements;
        for (int i = 0; i < 5; ++i) {
            const auto pair = settled_pair(QStringLiteral("paper"), i, 1.0);
            decisions.append(pair.first);
            settlements.append(pair.second);
        }
        const QJsonObject paper_verdict =
            KalshiBotGate::evaluate(params, decisions, settlements, kNow);
        const int paper_settled =
            paper_verdict.value(QStringLiteral("ledger")).toObject()
                .value(QStringLiteral("settled_bids")).toInt();
        QCOMPARE(paper_settled, 5);

        // Twenty live settlements, all profitable, must move NOTHING.
        for (int i = 100; i < 120; ++i) {
            const auto pair = settled_pair(QStringLiteral("live"), i, 5.0);
            decisions.append(pair.first);
            settlements.append(pair.second);
        }
        const QJsonObject mixed = KalshiBotGate::evaluate(params, decisions, settlements, kNow);
        const QJsonObject ledger = mixed.value(QStringLiteral("ledger")).toObject();
        QCOMPARE(ledger.value(QStringLiteral("settled_bids")).toInt(), paper_settled);
        QCOMPARE(ledger.value(QStringLiteral("net_pnl_usd")).toDouble(),
                 paper_verdict.value(QStringLiteral("ledger")).toObject()
                     .value(QStringLiteral("net_pnl_usd")).toDouble());
    }

    // ── the contract the venue already took is not bid again ────────────────

    // submit_order's per-contract duplicate guard counts drafts whose status is
    // in (submitting, submission_unknown, submitted) — but a LIVE submit writes
    // the VENUE's state onto the draft, so a filled bot order leaves a `filled`
    // draft the guard's IN-list misses. Without this the bot would re-buy the
    // contract it just bought, every tick, until a cap stopped it.
    void a_contract_the_venue_filled_is_not_quoted_again() {
        QJsonObject accepted = bid_row();
        accepted.insert(QStringLiteral("mode"), QStringLiteral("live"));
        accepted.insert(QStringLiteral("reason_code"),
                        QString::fromLatin1(KalshiBotLive::kLiveSubmitted));
        accepted.insert(QStringLiteral("submit_status"), QStringLiteral("filled"));
        const QJsonArray working = KalshiBotLive::live_working(QJsonArray{accepted});
        QCOMPARE(working.size(), 1);
        QCOMPARE(working.first().toObject().value(QStringLiteral("ticker")).toString(),
                 QStringLiteral("KXBTC15M-T1"));
    }

    // A bid the submit path refused left nothing at the venue — a rate limit, a
    // cap, a killed fill-and-kill. Blocking it would be the bot inventing a
    // position out of a rejection.
    void a_refused_bid_does_not_block_the_next_tick() {
        QJsonObject refused = bid_row();
        refused.insert(QStringLiteral("mode"), QStringLiteral("live"));
        refused.insert(QStringLiteral("reason_code"),
                       QString::fromLatin1(KalshiBotLive::kLiveRejected));
        refused.insert(QStringLiteral("submit_status"), QStringLiteral("rejected"));
        QVERIFY(KalshiBotLive::live_working(QJsonArray{refused}).isEmpty());
    }

    // A PAPER bid must not block a live tick: the two books are separate
    // everywhere else in this rung and must be here too. The row below carries
    // the live ACCEPTED reason code and differs only in `mode`, so `mode` is
    // the only thing that can be excluding it — a paper row with an ordinary
    // paper reason code would be excluded by the accepted-only filter instead,
    // and would never reach the check this test is named for.
    void a_paper_bid_does_not_block_a_live_tick() {
        QJsonObject paper = bid_row();
        paper.insert(QStringLiteral("reason_code"),
                     QString::fromLatin1(KalshiBotLive::kLiveSubmitted));
        QCOMPARE(paper.value(QStringLiteral("mode")).toString(), QStringLiteral("paper"));
        QVERIFY(KalshiBotLive::live_working(QJsonArray{paper}).isEmpty());
    }

    // The composition, not just the halves: live_working() feeding decide() is
    // what actually stops the bot re-buying the contract it just bought, and
    // the CLI's live tick is exactly this call. Tested here because a permitted
    // live tick cannot be reached from an e2e without arming for real.
    void the_bot_does_not_re_bid_the_contract_it_just_bought() {
        const QJsonObject report{
            {QStringLiteral("generated_at_ms"), static_cast<double>(kNow)},
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("predictions"),
             QJsonObject{{QStringLiteral("KXBTC15M-T1"),
                          QJsonObject{{QStringLiteral("p_yes_full"), 0.62},
                                      {QStringLiteral("market_yes_mid"), 0.40},
                                      {QStringLiteral("features"),
                                       QJsonObject{{QStringLiteral("sqrt_minutes_left"),
                                                    std::sqrt(10.0)}}}}}}}};

        // Control: with an empty book the same report DOES bid, so the pass
        // below cannot be attributed to the report failing some other gate.
        const QJsonArray fresh =
            KalshiBotDecision::decide(report, {}, {}, kNow, KalshiBotDecision::Config{}, {});
        QCOMPARE(fresh.size(), 1);
        QCOMPARE(fresh.first().toObject().value(QStringLiteral("action")).toString(),
                 QStringLiteral("bid"));

        QJsonObject accepted = bid_row();
        accepted.insert(QStringLiteral("mode"), QStringLiteral("live"));
        accepted.insert(QStringLiteral("reason_code"),
                        QString::fromLatin1(KalshiBotLive::kLiveSubmitted));
        const QJsonArray rows =
            KalshiBotDecision::decide(report, KalshiBotLive::live_working(QJsonArray{accepted}), {},
                                      kNow, KalshiBotDecision::Config{}, {});
        QCOMPARE(rows.size(), 1);
        const QJsonObject row = rows.first().toObject();
        QCOMPARE(row.value(QStringLiteral("action")).toString(), QStringLiteral("pass"));
        QCOMPARE(row.value(QStringLiteral("reason_code")).toString(),
                 QString::fromLatin1(KalshiBotDecision::kAlreadyHeld));
    }

    void a_row_without_a_mode_is_paper() {
        QJsonObject legacy = bid_row();
        legacy.remove(QStringLiteral("mode"));
        QVERIFY2(!KalshiBotLive::is_live_row(legacy),
                 "rung 1 wrote rows before modes existed; they are paper");
        QVERIFY(!KalshiBotLive::is_live_row(bid_row()));
        QJsonObject live = bid_row();
        live.insert(QStringLiteral("mode"), QStringLiteral("live"));
        QVERIFY(KalshiBotLive::is_live_row(live));
    }
};

QTEST_MAIN(TstKalshiBotLive)
#include "tst_kalshi_bot_live.moc"
