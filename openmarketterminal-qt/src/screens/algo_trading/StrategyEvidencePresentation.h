#pragma once

namespace openmarketterminal::screens {

enum class StrategyProofState {
    Collecting,
    NoEdge,
    Blocked,
    Hypothetical,
    PromotionReady,
};

inline StrategyProofState strategy_proof_state(bool hypothetical, bool eligible,
                                                int resolved, double net_pnl,
                                                int minimum_sample) {
    if (hypothetical)
        return StrategyProofState::Hypothetical;
    if (eligible)
        return StrategyProofState::PromotionReady;
    if (resolved >= minimum_sample && net_pnl <= 0.0)
        return StrategyProofState::NoEdge;
    if (resolved >= minimum_sample)
        return StrategyProofState::Blocked;
    return StrategyProofState::Collecting;
}

} // namespace openmarketterminal::screens
