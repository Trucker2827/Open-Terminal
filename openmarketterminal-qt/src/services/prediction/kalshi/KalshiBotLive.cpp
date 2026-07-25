#include "services/prediction/kalshi/KalshiBotLive.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace openmarketterminal::services::prediction::kalshi_ns {
namespace {

constexpr auto kDecisionEvent = "kalshi_bot_decision";

/// The charter's hard ceilings. A session may only tighten them, so what the
/// bot asks for is the MINIMUM of what the session stated and these. This is
/// not a second gate — `submit_order` re-checks the same limits from the
/// immutable draft and is the authority — it is the bot refusing to ASK for
/// more than the carve-out ever allows, even if a status object it is handed
/// says otherwise.
constexpr double kStakeCeilingUsd = 2.00;
constexpr double kAllInCeilingUsd = 3.00;
constexpr double kExperimentCeilingUsd = 120.00;
constexpr int kOrdersPerHourCeiling = 10;

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

double positive_number(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) return -1.0;
    const double number = value.toDouble();
    return number > 0.0 ? number : -1.0;
}

using Permission = KalshiBotLive::Permission;

Permission refuse(Permission permission, const char* code, const QString& detail) {
    permission.permitted = false;
    permission.reason_code = QString::fromLatin1(code);
    permission.detail = detail;
    return permission;
}

} // namespace

KalshiBotLive::Permission KalshiBotLive::permit(const QJsonObject& live_status,
                                                const QJsonObject& gate,
                                                const KalshiBotStopFile& stop,
                                                qint64 now_ms,
                                                qint64 max_gate_age_ms) {
    Permission permission;

    // (0) The kill switch outranks everything, exactly as it does inside
    //     decide(). A stopped bot is not admitted to live mode at all.
    if (stop.engaged)
        return refuse(permission, kRefusedStopped,
                      QStringLiteral("the kill switch is engaged — clear it with `kalshi bot "
                                     "resume` before any live mode is possible"));

    // (1) An arm state that could not be read is neither armed nor disarmed.
    //     Fail closed and say which, so nobody reads the refusal as "disarmed".
    if (live_status.isEmpty())
        return refuse(permission, kRefusedStatusUnknown,
                      QStringLiteral("`kalshi auto live status` could not be read, so the armed "
                                     "state is unknown — live mode fails closed"));

    const QJsonObject session = live_status.value(QStringLiteral("session")).toObject();
    permission.session_id = session.value(QStringLiteral("session_id")).toString();
    permission.session_ends_at = session.value(QStringLiteral("ends_at")).toString();

    // Caps come from the session a human armed, clamped to the charter's
    // ceilings. A cap the status does not state is not defaulted into
    // existence — a default here would be authority nobody granted.
    const double stake = positive_number(live_status, QStringLiteral("per_bet_contract_stake_cap"));
    const double all_in = positive_number(live_status, QStringLiteral("per_bet_all_in_tolerance"));
    const double experiment = positive_number(live_status, QStringLiteral("experiment_cap"));
    const QJsonValue orders = live_status.value(QStringLiteral("max_orders_per_hour"));
    if (stake < 0.0 || all_in < 0.0 || experiment < 0.0 || !orders.isDouble() || orders.toInt() < 1)
        return refuse(permission, kRefusedStatusUnknown,
                      QStringLiteral("the live status object states no usable caps, so there is no "
                                     "session limit to bid inside"));
    permission.max_stake_usd = std::min(stake, kStakeCeilingUsd);
    permission.max_all_in_usd = std::min(all_in, kAllInCeilingUsd);
    permission.experiment_cap_usd = std::min(experiment, kExperimentCeilingUsd);
    permission.max_orders_per_hour = std::min(orders.toInt(), kOrdersPerHourCeiling);

    // (2) THE HUMAN ARM. Every one of these is set outside the bot's reach:
    //     the session file by `kalshi auto live session`, the trading and live
    //     gates and the kill switch by the GUI, the venue allow-list by
    //     settings. The bot can read them and refuse; it can set none of them.
    const auto flag = [&live_status](const QString& key) {
        return live_status.value(key).toBool();
    };
    if (!flag(QStringLiteral("session_active")) || !flag(QStringLiteral("live_armed")) ||
        !flag(QStringLiteral("trading_allowed")) || !flag(QStringLiteral("venue_allowed")) ||
        flag(QStringLiteral("kill_switch")) || permission.session_id.isEmpty())
        return refuse(permission, kRefusedNotArmed,
                      QStringLiteral("no human-armed live session (session_active=%1 live_armed=%2 "
                                     "trading_allowed=%3 venue_allowed=%4 kill_switch=%5 "
                                     "session_id=%6) — arming is a human act")
                          .arg(flag(QStringLiteral("session_active")) ? QStringLiteral("true")
                                                                      : QStringLiteral("false"),
                               flag(QStringLiteral("live_armed")) ? QStringLiteral("true")
                                                                  : QStringLiteral("false"),
                               flag(QStringLiteral("trading_allowed")) ? QStringLiteral("true")
                                                                       : QStringLiteral("false"),
                               flag(QStringLiteral("venue_allowed")) ? QStringLiteral("true")
                                                                     : QStringLiteral("false"),
                               flag(QStringLiteral("kill_switch")) ? QStringLiteral("true")
                                                                   : QStringLiteral("false"),
                               permission.session_id.isEmpty() ? QStringLiteral("(none)")
                                                               : permission.session_id));

    // (3) BOUNDED. `session_active` is true for a `24/7` arm, whose `ends_at`
    //     is an empty string — the terminal's activity test treats a missing
    //     end as "no expiry yet". The carve-out's first condition is a BOUNDED
    //     run, so an end that is not a real timestamp still in the future is
    //     refused here rather than ridden.
    const QDateTime ends = QDateTime::fromString(permission.session_ends_at, Qt::ISODateWithMs);
    if (!ends.isValid() || ends.toMSecsSinceEpoch() <= now_ms)
        return refuse(permission, kRefusedUnbounded,
                      QStringLiteral("the armed session is not bounded in time (ends_at=%1) — a "
                                     "bounded run is a carve-out condition, so arm 1h/6h/12h "
                                     "rather than 24/7")
                          .arg(permission.session_ends_at.isEmpty()
                                   ? QStringLiteral("(none — 24/7)")
                                   : permission.session_ends_at));

    // (4) THE PREREGISTERED GATE, as PUBLISHED. Rung 3's rule: read the
    //     verdict the gate wrote, never re-derive it, so the bot and the screen
    //     beside it can never disagree about whether the signal is promoted.
    if (gate.isEmpty())
        return refuse(permission, kRefusedGateMissing,
                      QStringLiteral("no %1 — the promotion gate has never scored the paper "
                                     "record, so the signal is not promoted")
                          .arg(QStringLiteral("kalshi-bot-gate.json")));
    const QString verdict = gate.value(QStringLiteral("verdict")).toString();
    if (!gate.value(QStringLiteral("evaluated")).toBool())
        return refuse(permission, kRefusedGateRefused,
                      QStringLiteral("the gate refused to evaluate (%1: %2) — a refusal carries no "
                                     "criteria at all, so it is not a PASS")
                          .arg(verdict.isEmpty() ? QStringLiteral("no verdict") : verdict,
                               gate.value(QStringLiteral("reason"))
                                   .toString(QStringLiteral("no reason given"))));
    if (verdict != QLatin1String("PASS"))
        return refuse(permission, kRefusedGateFail,
                      QStringLiteral("the preregistered gate reads %1, not PASS — an ungated "
                                     "signal trades paper only")
                          .arg(verdict.isEmpty() ? QStringLiteral("no verdict") : verdict));

    const QJsonValue gate_ts = gate.value(QStringLiteral("ts_ms"));
    permission.gate_ts_ms = gate_ts.isDouble() ? static_cast<qint64>(gate_ts.toDouble()) : -1;
    // An undated verdict is as unusable as an old one, and a verdict dated in
    // the future is mistrusted rather than clamped (the runtime chip's rule).
    if (permission.gate_ts_ms <= 0 || permission.gate_ts_ms > now_ms ||
        now_ms - permission.gate_ts_ms > max_gate_age_ms)
        return refuse(permission, kRefusedGateStale,
                      QStringLiteral("the PASS verdict is %1 — re-run `kalshi bot gate` so the "
                                     "verdict describes the record that exists now (limit %2s)")
                          .arg(permission.gate_ts_ms <= 0
                                   ? QStringLiteral("undated")
                                   : permission.gate_ts_ms > now_ms
                                         ? QStringLiteral("dated in the future")
                                         : QStringLiteral("%1s old")
                                               .arg((now_ms - permission.gate_ts_ms) / 1000))
                          .arg(max_gate_age_ms / 1000));

    permission.permitted = true;
    permission.detail =
        QStringLiteral("armed session %1 ends %2 · gate PASS as of %3 · caps: stake <= $%4 · "
                       "all-in <= $%5 · <= %6 orders/hour · experiment <= $%7")
            .arg(permission.session_id, permission.session_ends_at, iso(permission.gate_ts_ms))
            .arg(permission.max_stake_usd, 0, 'f', 2)
            .arg(permission.max_all_in_usd, 0, 'f', 2)
            .arg(permission.max_orders_per_hour)
            .arg(permission.experiment_cap_usd, 0, 'f', 2);
    return permission;
}

QJsonObject KalshiBotLive::live_intent(const QJsonObject& bid_row, const Permission& permission) {
    if (!permission.permitted) return {};
    if (bid_row.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) return {};
    const QString ticker = bid_row.value(QStringLiteral("ticker")).toString().trimmed();
    const double price = bid_row.value(QStringLiteral("price")).toDouble(0.0);
    const int contracts = bid_row.value(QStringLiteral("contracts")).toInt(0);
    const double fee = bid_row.value(QStringLiteral("fee_usd")).toDouble(-1.0);
    const double stake = bid_row.value(QStringLiteral("stake_usd")).toDouble(-1.0);
    const QString side = bid_row.value(QStringLiteral("side")).toString().trimmed().toUpper();
    // A row that cannot state its own contract, price, size, or fee produces no
    // order. `estimated_fee` in particular fails closed at submit, so guessing
    // one here would only convert a clean refusal into a rejection.
    if (ticker.isEmpty() || !(price > 0.0 && price < 1.0) || contracts < 1 || fee < 0.0 ||
        stake < 0.0 || (side != QStringLiteral("YES") && side != QStringLiteral("NO")))
        return {};

    const bool yes = side == QStringLiteral("YES");
    return QJsonObject{
        {QStringLiteral("asset_class"), QStringLiteral("prediction")},
        {QStringLiteral("venue"), QStringLiteral("kalshi")},
        {QStringLiteral("market_id"), ticker},
        {QStringLiteral("asset_id"),
         ticker + QLatin1Char(':') + (yes ? QStringLiteral("yes") : QStringLiteral("no"))},
        {QStringLiteral("outcome"), yes ? QStringLiteral("Yes") : QStringLiteral("No")},
        {QStringLiteral("side"), QStringLiteral("buy")},
        {QStringLiteral("contracts"), contracts},
        {QStringLiteral("limit_price"), price},
        // Fill-and-kill, deliberately. This rung gives the bot no live cancel
        // path and no live TTL sweep — rung 6's lifecycle is the PAPER book's.
        // An order allowed to rest would therefore be live exposure nothing was
        // managing, which is the exact failure rung 6 exists to retire. FAK
        // leaves nothing behind: it fills at or inside the approved limit, or
        // it is gone.
        {QStringLiteral("order_type"), QStringLiteral("fak")},
        {QStringLiteral("post_only"), false},
        {QStringLiteral("cancel_order_on_pause"), true},
        // Every limit below is the ARMED SESSION's, never the bot's paper
        // config: the bot cannot hand itself a wider cap than the arm granted.
        {QStringLiteral("max_live_stake"), permission.max_stake_usd},
        {QStringLiteral("max_live_all_in"), permission.max_all_in_usd},
        {QStringLiteral("experiment_loss_cap"), permission.experiment_cap_usd},
        {QStringLiteral("max_orders_per_hour"), permission.max_orders_per_hour},
        {QStringLiteral("estimated_fee"), fee},
        {QStringLiteral("estimated_total"), stake + fee},
        // The two tags that put this order under the micro-live gate stack in
        // submit_order: without them it is an ordinary draft and the experiment
        // caps do not run at all.
        {QStringLiteral("experiment_id"), QString::fromLatin1(kExperimentId)},
        {QStringLiteral("automation_session_id"), permission.session_id},
        {QStringLiteral("automation_session_ends_at"), permission.session_ends_at},
        {QStringLiteral("autonomous"), true},
        {QStringLiteral("evidence_decision_id"), bid_row.value(QStringLiteral("position_id"))},
        {QStringLiteral("decision_origin"), QStringLiteral("KALSHI_BOT")},
        {QStringLiteral("decision_rationale"),
         QStringLiteral("Kalshi bot rung 5: %1 %2 x%3 at $%4 (%5) — calibrated %6 vs market mid "
                        "%7; gate PASS, session %8")
             .arg(side, ticker)
             .arg(contracts)
             .arg(price, 0, 'f', 2)
             .arg(bid_row.value(QStringLiteral("reason_code")).toString())
             .arg(bid_row.value(QStringLiteral("calibrated_p")).toDouble(), 0, 'f', 4)
             .arg(bid_row.value(QStringLiteral("market_mid")).toDouble(), 0, 'f', 4)
             .arg(permission.session_id)},
        {QStringLiteral("decision_snapshot_json"),
         QString::fromUtf8(QJsonDocument(bid_row).toJson(QJsonDocument::Compact))}};
}

QJsonObject KalshiBotLive::live_row(const QJsonObject& decision_row,
                                    const Permission& permission) {
    QJsonObject row = decision_row;
    row.insert(QStringLiteral("mode"), QString::fromLatin1(kModeLive));
    row.insert(QStringLiteral("live_eligible"), true);
    row.insert(QStringLiteral("live_caps"),
               QJsonObject{{QStringLiteral("session_id"), permission.session_id},
                           {QStringLiteral("session_ends_at"), permission.session_ends_at},
                           {QStringLiteral("max_stake_usd"), permission.max_stake_usd},
                           {QStringLiteral("max_all_in_usd"), permission.max_all_in_usd},
                           {QStringLiteral("experiment_cap_usd"), permission.experiment_cap_usd},
                           {QStringLiteral("max_orders_per_hour"), permission.max_orders_per_hour},
                           {QStringLiteral("gate_ts_ms"),
                            static_cast<double>(permission.gate_ts_ms)}});
    return row;
}

QJsonObject KalshiBotLive::live_row(const QJsonObject& decision_row,
                                    const QJsonObject& submission,
                                    const Permission& permission) {
    QJsonObject row = live_row(decision_row, permission);

    // The paper fill model never described this order and must not appear to.
    // A live order's state, fill and remainder are the VENUE's answer or they
    // are absent; the paper TTL and the paper book's exposure arithmetic belong
    // to the paper ledger, not here.
    for (const char* key : {"order_state", "filled_count", "remaining_count", "ttl_ms",
                            "expires_at_ms", "exposure_after_usd", "exposure_cap_usd",
                            "fill_model", "fill_rule"})
        row.remove(QLatin1String(key));

    // What the decision math said, kept under its own name: the live reason
    // code is about the venue, and overwriting the paper one would erase why
    // the bid was made at all.
    const QString decided = decision_row.value(QStringLiteral("reason_code")).toString();
    if (!decided.isEmpty()) row.insert(QStringLiteral("decision_reason_code"), decided);

    const QString status = submission.value(QStringLiteral("status")).toString();
    // Exactly the states `normalized_kalshi_order_state` can report as "the
    // venue has this order". Anything else — including an EMPTY status, which
    // is what a failed tool call leaves — is not an accepted order, and the row
    // says rejected rather than assuming the order lives.
    const bool accepted = status == QLatin1String("filled") ||
                          status == QLatin1String("partially_filled") ||
                          status == QLatin1String("resting") ||
                          status == QLatin1String("accepted") ||
                          status == QLatin1String("submitted");
    row.insert(QStringLiteral("reason_code"),
               QString::fromLatin1(accepted ? kLiveSubmitted : kLiveRejected));
    row.insert(QStringLiteral("submit_status"),
               status.isEmpty() ? QStringLiteral("(no status returned)") : status);
    const QString reason = submission.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty()) row.insert(QStringLiteral("submit_reason"), reason);
    if (accepted) row.insert(QStringLiteral("order_state"), status);
    for (const char* key : {"filled_count", "remaining_count", "fill_price"}) {
        const QJsonValue value = submission.value(QLatin1String(key));
        if (value.isDouble()) row.insert(QLatin1String(key), value.toDouble());
    }
    return row;
}

QJsonObject KalshiBotLive::refusal_row(const Permission& permission, qint64 now_ms) {
    return QJsonObject{{QStringLiteral("event"), QString::fromLatin1(kDecisionEvent)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("mode"), QString::fromLatin1(kModeLive)},
                       // Refused, so NOT eligible — and no ticker, price, size,
                       // or cap: there was no decision to describe.
                       {QStringLiteral("live_eligible"), false},
                       {QStringLiteral("action"), QStringLiteral("pass")},
                       {QStringLiteral("reason_code"), permission.reason_code},
                       {QStringLiteral("detail"), permission.detail}};
}

bool KalshiBotLive::is_live_row(const QJsonObject& row) {
    return row.value(QStringLiteral("mode")).toString() == QLatin1String(kModeLive);
}

QJsonArray KalshiBotLive::live_working(const QJsonArray& ledger_rows) {
    QSet<QString> tickers;
    for (const auto& value : ledger_rows) {
        const QJsonObject row = value.toObject();
        if (!is_live_row(row)) continue;
        if (row.value(QStringLiteral("event")).toString() != QLatin1String(kDecisionEvent)) continue;
        if (row.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) continue;
        // Only what the venue took. A refused bid left nothing behind.
        if (row.value(QStringLiteral("reason_code")).toString() != QLatin1String(kLiveSubmitted))
            continue;
        const QString ticker = row.value(QStringLiteral("ticker")).toString();
        if (!ticker.isEmpty()) tickers.insert(ticker);
    }
    QJsonArray out;
    for (const QString& ticker : tickers)
        out.append(QJsonObject{{QStringLiteral("ticker"), ticker}});
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
