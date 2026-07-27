#include "services/prediction/kalshi/KalshiBotFunnel.h"

#include "services/prediction/kalshi/KalshiBotGate.h"
#include "services/prediction/kalshi/KalshiBotLive.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"

#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

QString str(const QJsonObject& row, const char* key) {
    return row.value(QLatin1String(key)).toString();
}

/// `min_settled_bids` out of the sealed params record, or -1 with `*why` set.
///
/// The seal is checked, not just the number. A tampered or unsealed params file
/// makes the gate refuse and publish no numbers at all; a funnel that read the
/// minimum out of it anyway would be pacing the operator toward a target the
/// gate itself will not honour.
int sealed_min_settled_bids(const QJsonValue& params_record, QString* why) {
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return -1;
    };
    if (params_record.isUndefined())
        return fail(QStringLiteral("no %1 — nothing was preregistered, so there is no gate to pace "
                                   "toward").arg(QString::fromLatin1(KalshiBotGate::kParamsFile)));
    if (!params_record.isObject())
        return fail(QStringLiteral("%1 is not a params record — the gate would refuse it, so no "
                                   "target is quoted from it")
                        .arg(QString::fromLatin1(KalshiBotGate::kParamsFile)));
    const QJsonObject record = params_record.toObject();
    if (!KalshiBotGate::seal_valid(record))
        return fail(QStringLiteral("%1 does not match its own seal — the gate refuses it as "
                                   "TAMPERED, so the minimum it states is not quoted here")
                        .arg(QString::fromLatin1(KalshiBotGate::kParamsFile)));
    const QJsonValue minimum =
        record.value(QStringLiteral("params")).toObject().value(QStringLiteral("min_settled_bids"));
    if (!minimum.isDouble())
        return fail(QStringLiteral("the sealed params carry no numeric min_settled_bids"));
    return minimum.toInt();
}

} // namespace

KalshiBotFunnel KalshiBotFunnel::measure(const QJsonArray& rows, const QJsonValue& params_record) {
    KalshiBotFunnel funnel;
    funnel.rows_read = static_cast<int>(rows.size());

    QSet<QString> settled_positions;
    QSet<QString> models;
    // Keyed by `quote_style`, and a QMap rather than a QHash because the
    // published order is part of the answer: whichever way the rows arrive, the
    // file and the rendered sentence must come out the same (#158's defect was
    // exactly an order-dependent disclosure).
    struct Tier {
        int fills = 0;
        QSet<QString> rules;
    };
    QMap<QString, Tier> tiers;
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        // Honesty rule 3, and the same filter (for the same reason) that lives
        // inside KalshiBotOrders::replay() and KalshiBotGate::evaluate(): a
        // live outcome is not part of the paper record this funnel measures,
        // and `resting_now` below comes from a replay that already skips them.
        if (KalshiBotLive::is_live_row(row)) {
            ++funnel.live_rows_skipped;
            continue;
        }
        const QString event = str(row, "event");
        const bool decision = event == QLatin1String(kDecisionEvent);
        const bool settlement = event == QLatin1String(kSettlementEvent);
        if (!decision && !settlement) continue;

        // Provenance spans the rows actually measured, so a truncated record
        // shows up as a short span rather than as a healthy funnel.
        const qint64 ts_ms = kalshi_bot_row_ts_ms(row);
        if (ts_ms > 0) {
            funnel.first_ts_ms = funnel.first_ts_ms == 0 ? ts_ms : std::min(funnel.first_ts_ms, ts_ms);
            funnel.last_ts_ms = std::max(funnel.last_ts_ms, ts_ms);
        }

        if (settlement) {
            // Counted exactly as KalshiBotGate::evaluate() counts a settled
            // bid: deduped by position_id, and a settlement it could not
            // identify is not a settled bid at all.
            const QString id = str(row, "position_id");
            if (id.isEmpty() || !row.value(QStringLiteral("realized_pnl")).isDouble()) {
                ++funnel.settlements_unidentifiable;
                continue;
            }
            if (settled_positions.contains(id)) continue;
            settled_positions.insert(id);
            ++funnel.settlements;
            continue;
        }

        const QString model = str(row, "fill_model");
        if (!model.isEmpty()) models.insert(model);

        const QString action = str(row, "action");
        if (action == QStringLiteral("bid")) {
            ++funnel.bids;
            // Rung 1 wrote no `order_state`; replay() books those bids as
            // assumed fills at the quoted price. They are NOT fills — nothing
            // observed them fill — so they are counted separately rather than
            // folded into either side of the fill rate.
            if (!row.contains(QStringLiteral("order_state"))) ++funnel.legacy_assumed_fill_bids;
        } else if (action == QStringLiteral("fill")) {
            ++funnel.fills;
            // Bucketed by the tier the DECIDING tick journaled on the order,
            // never by matching the rule's prose. Every fill lands in exactly
            // one bucket — including one carrying no rule at all — so the
            // per-tier counts sum to `fills` and cannot under-report a tier by
            // omitting the rows that stated nothing.
            const QString style = str(row, "quote_style");
            Tier& tier = tiers[style.isEmpty() ? QString::fromLatin1(kKalshiBotFunnelUnstatedTier)
                                               : style];
            ++tier.fills;
            const QString rule = str(row, "fill_rule");
            if (!rule.isEmpty()) tier.rules.insert(rule);
        } else if (action == QStringLiteral("cancel")) {
            const QString reason = str(row, "reason_code");
            if (reason == QLatin1String(KalshiBotOrders::kCanceledTtl))
                ++funnel.canceled_ttl;
            else if (reason == QLatin1String(KalshiBotOrders::kCanceledEdgeGone))
                ++funnel.canceled_edge_gone;
            else if (reason == QLatin1String(KalshiBotOrders::kCanceledSettled))
                ++funnel.canceled_market_settled;
            else if (reason == QLatin1String(KalshiBotOrders::kUnconfirmedCancel))
                ++funnel.unconfirmed_cancels;
        }
    }
    funnel.fill_models = QStringList(models.constBegin(), models.constEnd());
    funnel.fill_models.sort();

    // One entry per tier, in key order. A tier whose rows disagreed about the
    // rule quotes NO sentence: picking one of two contradicting disclosures is
    // the last-writer bug this whole structure replaces, one level down.
    for (auto it = tiers.constBegin(); it != tiers.constEnd(); ++it) {
        FillRule entry;
        entry.quote_style = it.key();
        entry.fills = it.value().fills;
        if (it.value().rules.size() == 1)
            entry.rule = *it.value().rules.constBegin();
        else if (it.value().rules.size() > 1)
            entry.rules_disagree = true;
        funnel.fill_rules.append(entry);
    }

    // One authority for "what is still working": the book replay the tick and
    // the exposure sum already use, over the same rows.
    funnel.resting_now =
        static_cast<int>(KalshiBotOrders::replay(rows).resting.size());

    if (funnel.last_ts_ms > funnel.first_ts_ms) funnel.span_ms = funnel.last_ts_ms - funnel.first_ts_ms;

    // Honesty rule 1: a rate with no denominator is absent, never 0.0.
    if (funnel.bids > 0) {
        funnel.fill_rate_available = true;
        funnel.fill_rate = static_cast<double>(funnel.fills) / funnel.bids;
    }
    if (funnel.span_ms > 0) {
        funnel.settled_per_day_available = true;
        funnel.settled_per_day =
            funnel.settlements * 86'400'000.0 / static_cast<double>(funnel.span_ms);
    }

    // --- the pace, and every reason it is not a promise ---------------------
    QString why;
    const int required = sealed_min_settled_bids(params_record, &why);
    if (required >= 0) {
        funnel.gate_required_available = true;
        funnel.gate_required = required;
    }
    if (required < 0) {
        funnel.pace_unavailable_reason = why;
    } else if (funnel.span_ms < kKalshiBotFunnelMinPaceSpanMs) {
        funnel.pace_unavailable_reason =
            QStringLiteral("the record spans %1 ms, under the one hour a settlement rate would have "
                           "to be projected from — no days figure is extrapolated from it")
                .arg(funnel.span_ms);
    } else if (!funnel.settled_per_day_available || funnel.settled_per_day <= 0.0) {
        funnel.pace_unavailable_reason =
            QStringLiteral("the observed settlement rate is zero over this record, and a gate is "
                           "not reachable at a rate of zero — no number is invented for it");
    } else {
        funnel.pace_available = true;
        funnel.settled_remaining = std::max(0, required - funnel.settlements);
        funnel.days_to_gate_at_observed_rate = funnel.settled_remaining / funnel.settled_per_day;
    }
    return funnel;
}

QJsonObject KalshiBotFunnel::to_json(qint64 now_ms) const {
    QJsonObject out{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("event"), QStringLiteral("kalshi_bot_funnel")},
        {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ts"),
         QDateTime::fromMSecsSinceEpoch(now_ms, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
        {QStringLiteral("bids"), bids},
        {QStringLiteral("resting_now"), resting_now},
        {QStringLiteral("fills"), fills},
        {QStringLiteral("canceled_ttl"), canceled_ttl},
        {QStringLiteral("canceled_edge_gone"), canceled_edge_gone},
        {QStringLiteral("canceled_market_settled"), canceled_market_settled},
        {QStringLiteral("unconfirmed_cancels"), unconfirmed_cancels},
        {QStringLiteral("settlements"), settlements},
        {QStringLiteral("settlements_unidentifiable"), settlements_unidentifiable},
        {QStringLiteral("legacy_assumed_fill_bids"), legacy_assumed_fill_bids},
        // Provenance: what was read, and over what stretch of time.
        {QStringLiteral("rows_read"), rows_read},
        {QStringLiteral("live_rows_skipped"), live_rows_skipped},
        {QStringLiteral("first_ts_ms"), static_cast<double>(first_ts_ms)},
        {QStringLiteral("last_ts_ms"), static_cast<double>(last_ts_ms)},
        {QStringLiteral("span_ms"), static_cast<double>(span_ms)},
        {QStringLiteral("fill_models"), QJsonArray::fromStringList(fill_models)}};
    // One disclosure per tier, keyed by the tier. A missing `rule` is a MISSING
    // KEY, exactly like every other absent number here: a tier whose rows
    // carried no sentence, or two contradicting ones, publishes no sentence.
    if (!fill_rules.isEmpty()) {
        QJsonObject rules;
        for (const FillRule& tier : fill_rules) {
            QJsonObject entry{{QStringLiteral("fills"), tier.fills}};
            if (!tier.rule.isEmpty()) entry.insert(QStringLiteral("rule"), tier.rule);
            if (tier.rules_disagree) entry.insert(QStringLiteral("rules_disagree"), true);
            rules.insert(tier.quote_style, entry);
        }
        out.insert(QStringLiteral("fill_rules"), rules);
    }
    // Absent is an ABSENT KEY. A reader that finds no `fill_rate` has nothing
    // to print; a 0.0 would read as "nothing ever fills", which is a different
    // and unsupported claim.
    if (fill_rate_available) out.insert(QStringLiteral("fill_rate"), fill_rate);
    if (settled_per_day_available) out.insert(QStringLiteral("settled_per_day"), settled_per_day);
    if (gate_required_available) out.insert(QStringLiteral("gate_required"), gate_required);
    if (pace_available) {
        out.insert(QStringLiteral("settled_remaining"), settled_remaining);
        out.insert(QStringLiteral("days_to_gate_at_observed_rate"), days_to_gate_at_observed_rate);
    } else {
        out.insert(QStringLiteral("pace_unavailable_reason"), pace_unavailable_reason);
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
