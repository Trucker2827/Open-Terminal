#include "services/ai_strategy/TypedAction.h"

#include <algorithm>

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
                                            const ActionParams& params) {
    switch (choice.action) {
        case ActionType::Enter: {
            const double q = std::clamp(choice.conviction, 0.0, 1.0) * params.max_qty;
            if (q <= 0.0)
                return std::nullopt;
            return buy(choice.symbol, q);
        }
        case ActionType::Trim: {
            if (current_net_qty <= 0.0)
                return std::nullopt;
            const double q = current_net_qty * params.trim_fraction;
            if (q <= 0.0)
                return std::nullopt;
            return sell(choice.symbol, q);
        }
        case ActionType::Exit: {
            if (current_net_qty <= 0.0)
                return std::nullopt;
            return sell(choice.symbol, current_net_qty);
        }
        case ActionType::Skip:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ai_strategy
} // namespace openmarketterminal
