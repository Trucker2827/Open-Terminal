"""Which contracts the trust flag is allowed to be measured on.

`adds_value_over_market` decides whether the bot bids. It was measured over
every resolved contract -- a population dominated by far-from-strike contracts
where both the model and the market are nearly always right. The bot does not
bet that population: it bets where its edge over the mid clears the bid
threshold, which is near the money, and there the market has been winning.

This module owns the rule, once, because three calibrators need it and three
copies of a safety-critical constant drift.

It owns the PREDICATE only. The three calibrators do not share a scoring shape
-- spot_calibrator compares one model against the mid, while the two 15-minute
calibrators score several physics variants and select a trusted one -- so each
applies this predicate inside its own structure.
"""

# Mirrors KalshiBotDecision::Config::edge_threshold
# (src/services/prediction/kalshi/KalshiBotDecision.h:236). The C++ value is a
# struct default and cannot be read from here, so the two MUST be moved
# together by hand. A silent mismatch would score a population the bot does not
# actually bet, which is the exact defect this module exists to fix.
BET_EDGE_THRESHOLD = 0.10

# Same floor as the calibrators' MIN_SCORED_CONTRACTS, applied to the harder
# population. Deliberately not lowered: a smaller sample is a noisier estimate,
# and near-money contracts sit close to a coin flip, which is where a lucky
# streak impersonates skill.
MIN_ELIGIBLE_CONTRACTS = 100


def is_eligible(model_p, yes_mid):
    """True when the model's edge over the mid reaches the bot's bid threshold.

    Symmetric on purpose: a model below the mid by the threshold is as bettable
    as one above it, because the bot simply bids the other side.
    """
    if model_p is None or yes_mid is None:
        return False
    # Use small epsilon for floating point comparison to handle precision issues
    # (e.g., 0.72 - 0.62 = 0.09999999999999998 due to float representation)
    return abs(float(model_p) - float(yes_mid)) >= BET_EDGE_THRESHOLD - 1e-9


def eligible_pairs(rows, outcome):
    """Split one contract's (model_p, yes_mid) rows into scoring pairs.

    Returns `(model_pairs, mid_pairs)` over the ELIGIBLE observations only, each
    shaped `(p, outcome)` for the callers' existing `brier()`. A contract with no
    eligible observation returns two empty lists and must not be scored at all --
    it is not evidence about betting, because no bet was available.
    """
    model_pairs = []
    mid_pairs = []
    for model_p, yes_mid in rows:
        if not is_eligible(model_p, yes_mid):
            continue
        model_pairs.append((model_p, outcome))
        mid_pairs.append((yes_mid, outcome))
    return model_pairs, mid_pairs


def adds_value(model_scores, mid_scores):
    """Fail-closed verdict over per-contract eligible Brier scores."""
    if not model_scores or not mid_scores:
        return False
    if len(model_scores) < MIN_ELIGIBLE_CONTRACTS:
        return False
    return (sum(model_scores) / len(model_scores)) < (sum(mid_scores) / len(mid_scores))
