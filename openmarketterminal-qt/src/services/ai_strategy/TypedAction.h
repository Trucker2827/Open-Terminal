#pragma once
#include "services/ai_strategy/Strategy.h"  // TradeIntent = QJsonObject

#include <QString>
#include <optional>

namespace openmarketterminal {
namespace ai_strategy {

enum class ActionType { Skip, Enter, Trim, Exit };

/// One typed verb the LLM emits for a symbol.
struct ActionChoice {
    QString symbol;
    ActionType action = ActionType::Skip;
    double conviction = 1.0;   // 0..1, relevant to Enter; default full.
};

struct ActionParams {
    double max_qty = 10.0;       // enter full-conviction size.
    double trim_fraction = 0.5;  // fraction of the current position a Trim sells.
};

/// Pure: no DB, no LLM. Turns a verb + the symbol's CURRENT signed ledger position
/// into a paper TradeIntent, or std::nullopt (skip / nothing to do). Long-only baseline:
/// Enter buys conviction*max_qty (market); Trim sells trim_fraction of a long; Exit closes
/// a long; flat/short positions yield nothing for Trim/Exit.
std::optional<TradeIntent> translate_action(const ActionChoice& choice, double current_net_qty,
                                            const ActionParams& params);

} // namespace ai_strategy
} // namespace openmarketterminal
