#include "services/ai_strategy/DeterministicFloor.h"

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

} // namespace ai_strategy
} // namespace openmarketterminal
