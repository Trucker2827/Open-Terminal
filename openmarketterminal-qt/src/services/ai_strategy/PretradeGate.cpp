#include "services/ai_strategy/PretradeGate.h"
#include "services/ai_strategy/DeterministicFloor.h"

#include <cmath>

namespace openmarketterminal::ai_strategy {

GateVerdict evaluate_pretrade(const TradeIntent& intent, const GateInputs& in, const GatePolicy& policy) {
    const double qty = intent.value(QStringLiteral("quantity")).toDouble();
    if (qty <= 0.0)
        return {false, QStringLiteral("non-positive quantity"), QStringLiteral("position")};
    if (in.resolved_price <= 0.0)
        return {false, QStringLiteral("no price to evaluate"), QStringLiteral("cost")};
    const bool de_risking = intent_reduces_exposure(intent, in.existing_net_qty);
    if (!de_risking && policy.require_cost_gate && in.clears_cost != QStringLiteral("true"))
        return {false, QStringLiteral("cost not affirmatively clear"), QStringLiteral("cost")};
    if (!de_risking && policy.require_freshness_gate && in.freshness != QStringLiteral("ok"))
        return {false, QStringLiteral("freshness not affirmatively ok"), QStringLiteral("freshness")};
    if (policy.max_notional_per_order > 0.0 && qty * in.resolved_price > policy.max_notional_per_order) {
        const QString side = intent.value(QStringLiteral("side")).toString();
        const double signed_new =
            (side == QLatin1String("sell") || side == QLatin1String("short")) ? -qty : qty;
        const double resulting = in.existing_net_qty + signed_new;
        // De-risking exemption (mirrors the position/aggregate caps): a reduce/close
        // that does not grow absolute exposure is never blocked — a full exit must not
        // be rejected for size. Only an EXPOSURE-GROWING order is notional-capped.
        if (std::abs(resulting) > std::abs(in.existing_net_qty))
            return {false, QStringLiteral("notional exceeds cap"), QStringLiteral("notional")};
    }
    if (policy.max_position_qty > 0.0) {
        const QString side = intent.value(QStringLiteral("side")).toString();
        const double signed_new =
            (side == QLatin1String("sell") || side == QLatin1String("short")) ? -qty : qty;
        const double resulting = in.existing_net_qty + signed_new;
        // Increase-only: reject only when the intent grows absolute exposure beyond the cap.
        // A reduce/close/flip-that-reduces (|resulting| <= |existing|) always passes.
        if (std::abs(resulting) > policy.max_position_qty && std::abs(resulting) > std::abs(in.existing_net_qty))
            return {false, QStringLiteral("position would exceed cap"), QStringLiteral("position")};
    }
    if (policy.max_aggregate_position_qty > 0.0) {
        const QString side = intent.value(QStringLiteral("side")).toString();
        const double signed_new =
            (side == QLatin1String("sell") || side == QLatin1String("short")) ? -qty : qty;
        const double agg_resulting = in.aggregate_net_qty + signed_new;
        // Increase-only on the AGGREGATE (all handlers) exposure; a reduce/close always passes.
        if (std::abs(agg_resulting) > policy.max_aggregate_position_qty &&
            std::abs(agg_resulting) > std::abs(in.aggregate_net_qty))
            return {false, QStringLiteral("aggregate position would exceed cap"), QStringLiteral("aggregate")};
    }
    if (!policy.allowed_venues.isEmpty()) {
        QString v = intent.value(QStringLiteral("venue")).toString().trimmed().toLower();
        if (v.isEmpty())
            v = intent.value(QStringLiteral("exchange")).toString().trimmed().toLower();
        if (v.isEmpty())
            return {false, QStringLiteral("venue required by policy"), QStringLiteral("venue")};

        bool allowed = false;
        for (const QString& configured : policy.allowed_venues) {
            if (configured.trimmed().toLower() == v) {
                allowed = true;
                break;
            }
        }
        if (!allowed)
            return {false, QStringLiteral("venue not allowed"), QStringLiteral("venue")};
    }
    return {true, {}, {}};
}

} // namespace openmarketterminal::ai_strategy
