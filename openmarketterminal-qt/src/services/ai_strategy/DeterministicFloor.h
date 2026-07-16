#pragma once
#include "services/ai_strategy/PretradeGate.h"  // GateVerdict

#include <QString>

namespace openmarketterminal {
namespace ai_strategy {

/// The honest-edge signals the floor keys on (from a symbol's DecisionPacket).
struct FloorInputs {
    bool has_edge_signal = false;
    QString gate;         ///< packet.gate; "pass" is the only positive endorsement.
    QString clears_cost;  ///< "true" | "false" | "unknown".
    QString freshness;    ///< "ok" | "degraded" | "unknown".
};

struct FloorPolicy {
    bool require_floor = true;  ///< false => floor always permits (pass-through).
};

/// Pure: no DB, no settings, no I/O — reads only its arguments (mirrors evaluate_pretrade).
/// Affirmative / fail-closed: permits iff the floor is disabled, OR the honest edge system
/// positively endorses the symbol (has_edge_signal && gate=="pass" && clears_cost=="true"
/// && freshness != "degraded"). Otherwise rejects with rule="floor" and a specific reason.
GateVerdict floor_verdict(const FloorInputs& in, const FloorPolicy& policy);

} // namespace ai_strategy
} // namespace openmarketterminal
