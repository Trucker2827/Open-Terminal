#include "services/ai_strategy/TypedAction.h"

#include <algorithm>
#include <cmath>

namespace openmarketterminal {
namespace ai_strategy {

namespace {
TradeIntent buy(const QString& sym, double qty) {
    return TradeIntent{{"symbol", sym}, {"side", "buy"}, {"quantity", qty}, {"order_type", "market"}};
}
TradeIntent sell(const QString& sym, double qty) {
    return TradeIntent{{"symbol", sym}, {"side", "sell"}, {"quantity", qty}, {"order_type", "market"}};
}
}  // namespace

std::optional<TradeIntent> translate_action(const ActionChoice& choice, double current_net_qty,
                                            int edge_direction, const ActionParams& params) {
    switch (choice.action) {
        case ActionType::Enter: {
            if (edge_direction == 0)
                return std::nullopt;  // missing/stale/neutral/conflicting edge -> no entry
            // No one-step reversal: an Enter opposing a non-flat position is rejected;
            // the position must be reduced/closed first (Trim/Exit) in an earlier cycle.
            if (current_net_qty != 0.0 &&
                ((current_net_qty > 0.0 ? 1 : -1) != edge_direction))
                return std::nullopt;
            const double q = std::clamp(choice.conviction, 0.0, 1.0) * params.max_qty;
            if (q <= 0.0)
                return std::nullopt;
            return edge_direction > 0 ? buy(choice.symbol, q) : sell(choice.symbol, q);
        }
        case ActionType::Trim: {
            if (current_net_qty == 0.0)
                return std::nullopt;
            const double q = std::abs(current_net_qty) * params.trim_fraction;
            if (q <= 0.0)
                return std::nullopt;
            return current_net_qty > 0.0 ? sell(choice.symbol, q) : buy(choice.symbol, q);
        }
        case ActionType::Exit: {
            if (current_net_qty == 0.0)
                return std::nullopt;
            const double q = std::abs(current_net_qty);
            return current_net_qty > 0.0 ? sell(choice.symbol, q) : buy(choice.symbol, q);
        }
        case ActionType::Skip:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ai_strategy
} // namespace openmarketterminal
