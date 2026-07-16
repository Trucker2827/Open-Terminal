#include "services/ai_strategy/DeterministicFloor.h"

#include <cmath>

namespace openmarketterminal {
namespace ai_strategy {

GateVerdict floor_verdict(const FloorInputs& in, const FloorPolicy& policy) {
    if (!policy.require_floor)
        return {true, {}, {}};
    if (!in.has_edge_signal)
        return {false, QStringLiteral("no edge signal"), QStringLiteral("floor")};
    if (in.gate != QLatin1String("pass"))
        return {false, QStringLiteral("edge gate not pass"), QStringLiteral("floor")};
    if (in.clears_cost != QLatin1String("true"))
        return {false, QStringLiteral("below cost (not affirmatively clear)"), QStringLiteral("floor")};
    if (in.freshness == QLatin1String("degraded"))
        return {false, QStringLiteral("stale data"), QStringLiteral("floor")};
    return {true, {}, {}};
}

bool intent_reduces_exposure(const TradeIntent& intent, double existing_net_qty) {
    if (existing_net_qty == 0.0)
        return false;  // nothing to de-risk; opening needs endorsement.
    const QString side = intent.value(QStringLiteral("side")).toString();
    const double qty = intent.value(QStringLiteral("quantity")).toDouble();
    const double signed_new =
        (side == QLatin1String("sell") || side == QLatin1String("short")) ? -qty : qty;
    return std::abs(existing_net_qty + signed_new) <= std::abs(existing_net_qty);
}

} // namespace ai_strategy
} // namespace openmarketterminal
