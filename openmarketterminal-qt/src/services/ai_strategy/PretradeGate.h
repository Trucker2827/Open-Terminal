#pragma once
// PretradeGate.h — pure pre-trade guardrail (piece ③ of the AI decision
// surface, PAPER-ONLY/SUBTRACTIVE).
//
// evaluate_pretrade(intent, GateInputs, GatePolicy) -> GateVerdict is the
// pure core: given a trade intent plus its already-computed cost/freshness
// verdicts and the active policy, it returns allow / reject+reason+rule.
// NO DB, NO settings, no I/O — it only reads its arguments.
#include "services/ai_strategy/Strategy.h"

#include <QString>
#include <QStringList>

namespace openmarketterminal::ai_strategy {

struct GateVerdict {
    bool ok = true;
    QString reason;
    QString rule;
};

struct GateInputs {
    double resolved_price = 0.0;
    QString clears_cost;
    QString freshness;
    bool has_edge_signal = false;
};

struct GatePolicy {
    double max_notional_per_order = 0.0;
    double max_position_qty = 0.0;
    QStringList allowed_venues;
    bool require_cost_gate = true;
    bool require_freshness_gate = true;
};

GateVerdict evaluate_pretrade(const TradeIntent& intent, const GateInputs& in, const GatePolicy& policy);

} // namespace openmarketterminal::ai_strategy
