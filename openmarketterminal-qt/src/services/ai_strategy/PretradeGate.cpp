#include "services/ai_strategy/PretradeGate.h"

namespace openmarketterminal::ai_strategy {

GateVerdict evaluate_pretrade(const TradeIntent& intent, const GateInputs& in, const GatePolicy& policy) {
    const double qty = intent.value(QStringLiteral("quantity")).toDouble();
    if (qty <= 0.0)
        return {false, QStringLiteral("non-positive quantity"), QStringLiteral("position")};
    if (in.resolved_price <= 0.0)
        return {false, QStringLiteral("no price to evaluate"), QStringLiteral("cost")};
    if (policy.require_cost_gate && in.clears_cost == QStringLiteral("false"))
        return {false, QStringLiteral("below cost"), QStringLiteral("cost")};
    if (policy.require_freshness_gate && in.freshness == QStringLiteral("degraded"))
        return {false, QStringLiteral("stale data"), QStringLiteral("freshness")};
    if (policy.max_notional_per_order > 0.0 && qty * in.resolved_price > policy.max_notional_per_order)
        return {false, QStringLiteral("notional exceeds cap"), QStringLiteral("notional")};
    if (policy.max_position_qty > 0.0 && qty > policy.max_position_qty)
        return {false, QStringLiteral("quantity exceeds cap"), QStringLiteral("position")};
    if (!policy.allowed_venues.isEmpty()) {
        QString v = intent.value(QStringLiteral("venue")).toString();
        if (v.isEmpty()) v = intent.value(QStringLiteral("exchange")).toString();
        if (!v.isEmpty() && !policy.allowed_venues.contains(v))
            return {false, QStringLiteral("venue not allowed"), QStringLiteral("venue")};
    }
    return {true, {}, {}};
}

} // namespace openmarketterminal::ai_strategy
