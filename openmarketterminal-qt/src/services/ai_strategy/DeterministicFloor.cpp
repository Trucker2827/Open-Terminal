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
    if (in.freshness != QLatin1String("ok"))
        return {false, QStringLiteral("freshness not affirmatively ok"), QStringLiteral("floor")};
    return {true, {}, {}};
}

bool intent_reduces_exposure(const TradeIntent& intent, double existing_net_qty) {
    if (existing_net_qty == 0.0)
        return false;  // nothing to de-risk; opening needs endorsement.
    const QString side = intent.value(QStringLiteral("side")).toString();
    const double qty = intent.value(QStringLiteral("quantity")).toDouble();
    const double signed_new =
        (side == QLatin1String("sell") || side == QLatin1String("short")) ? -qty : qty;
    const double resulting = existing_net_qty + signed_new;
    // A pure reduction stays on the same side (or closes to zero) AND shrinks magnitude.
    // A flip-through-zero (sign change to a nonzero opposite) is a REVERSAL, not de-risking —
    // it opens a new position and must NOT be floor-exempt.
    return resulting * existing_net_qty >= 0.0 && std::abs(resulting) < std::abs(existing_net_qty);
}

// Normalize a heterogeneous side/direction token to a trade direction:
// +1 long, -1 short, 0 neutral/unknown. Long tokens: buy/long. Short: sell/short.
// Everything else (avoid_buy, hold, flat, yes, no, none, "", unknown) -> 0.
int side_direction(const QString& s) {
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("buy") || t == QLatin1String("long"))
        return 1;
    if (t == QLatin1String("sell") || t == QLatin1String("short"))
        return -1;
    return 0;
}

bool intent_agrees_with_edge(const QString& intent_side, const QString& edge_side) {
    const int i = side_direction(intent_side);
    const int e = side_direction(edge_side);
    // Fail-closed: BOTH must be a definite, MATCHING direction. A neutral/unknown
    // edge side (avoid_buy/hold/flat/yes/no/empty) never affirmatively endorses,
    // so a long-only enter on a short/neutral edge is skipped by the floor.
    return i != 0 && e != 0 && i == e;
}

} // namespace ai_strategy
} // namespace openmarketterminal
