# Score the calibrator's trust flag on the bet-eligible subset

**Date:** 2026-08-09
**Status:** approved, not yet implemented

## The problem

`adds_value_over_market` decides whether the bot bids at all. `KalshiBotDecision::signal_trusted()`
reads it, and an untrusted signal journals `SIGNAL_UNTRUSTED` and places no order.

The calibrator sets that flag by comparing its per-contract Brier against the raw market mid over
**every** resolved contract — currently 500. That population is dominated by far-from-strike
contracts where both the model and the market are nearly always right, so the comparison is easy
and the margin is thin:

```
brier_full 0.06767  vs  brier_market_mid_raw 0.06897     ->  adds_value_over_market = true
```

The bot does not bet that population. It bets where its edge over the mid clears `edge_threshold`,
which is near the money. The sealed gate already scores exactly those contracts, and there the
result reverses:

```
brier_beats_market: observed 0.2576  vs  required (market) 0.2130   ->  FAIL
```

So the flag that authorises betting is measured on a population the bot does not bet, and on the
population it does bet the market wins by 0.045. That is why the signal can read "trusted" while
the paper book runs at 50W/50L with avg win $0.85 against avg loss $1.69 — a 1.98x payoff ratio
needing a 66.4% win rate to break even.

This is a measurement defect, not a modelling one. The fix is to measure the claim on the
population the claim is used for.

## Decisions taken

| Question | Decision |
|---|---|
| What counts as bet-eligible | The model's edge over the market mid reached the bot's own bid threshold (0.10) |
| Expected outcome | The bot goes silent until the model beats the mid where it acts. That is the criterion working, not a regression |
| Which calibrators | All three: `spot_calibrator`, `kxbtc15m_calibrator`, `commodities_15m_calibrator` |
| Sample-size floor | 100 **eligible** contracts — same bar as today, applied to the harder population |

The floor is deliberately not lowered. A smaller sample is a noisier estimate, so it needs at least
as much evidence, not less; and near-money contracts sit close to a coin flip, which is exactly
where a lucky streak impersonates skill.

## Architecture

Two halves, deliberately separated.

**Measurement lives in Python.** A new `scripts/kalshi_advise/calibrator_eligibility.py` owns the
eligibility rule and the threshold constant. All three calibrators import it. The rule is written
once because three copies of a safety-critical constant drift, and drift here means a family
silently trusting itself on the wrong population.

The shared module owns the **predicate only** — the threshold and `is_eligible(model_p, yes_mid)`.
It does not own the accumulators, because the three calibrators are not the same shape:

- `spot_calibrator` compares one full model against the mid, and sets
  `adds_value_over_market = b_full < b_mid_raw`.
- `kxbtc15m_calibrator` and `commodities_15m_calibrator` score several **physics variants**, select
  a `trusted_variant`, and set the flag from whether any variant earned trust
  (`adds_value`, `trusted is not None`).

Pasting one accumulator across both shapes would either break variant selection or quietly score
the wrong thing. Each calibrator therefore applies the shared predicate inside its own existing
structure. For the variant-based two, that means **every variant is scored on the eligible subset,
and `trusted_variant` may only be set by a variant that beats the mid there** — the same rule, in
the shape that calibrator already uses.

**Enforcement lives in C++.** `KalshiBotDecision::signal_trusted()` is already the single chokepoint
for trust, already documented, already tested. The new requirement lands there.

This split means the Python change is purely additive — it publishes new numbers and changes no
existing field — so it can be deployed and observed before the C++ change alters any behaviour.

## Scoring rule

Eligibility is a property of an **observation**; scoring is deliberately per **contract**.

The existing rule is one Brier score per contract, averaged over that contract's observations. That
is not incidental: observations inside one market are correlated, and pooling them would inflate
the effective sample size. The project's own method notes record this ("one observation per market,
or cluster the SE").

The new score keeps one score per contract, but computes it over **only that contract's eligible
observations**:

```
eligible(obs)  ==  abs(p(obs) - obs["yes_mid"]) >= BET_EDGE_THRESHOLD

contract_score_eligible_model = brier([(p(o), outcome) for o in eligible_obs])
contract_score_eligible_mid   = brier([(o["yes_mid"], outcome) for o in eligible_obs])
```

`p` is the same predictor the calibrator already scores against the mid, taken at the same point in
the walk-forward replay — `full.predict(f)` in `spot_calibrator`, and the variant under test in the
two variant-based calibrators. Eligibility is therefore evaluated per predictor: a contract can be
eligible for one variant and not another, which is correct, because the bot would only have bet the
variant whose edge cleared the threshold.

A contract with no eligible observation is not scored at all — it contributes to neither numerator
nor denominator. It is not evidence about betting, because no bet would have been available.

This measures the model at the moments it would have acted, without re-importing the pooling
inflation the current design carefully avoids.

## The threshold constant

`BET_EDGE_THRESHOLD = 0.10` in the new Python module, mirroring
`KalshiBotDecision::Config::edge_threshold` (`KalshiBotDecision.h:236`).

The C++ value is a struct default and cannot be read from Python. The constant is therefore
duplicated by necessity, and the duplication is made loud rather than quiet: the Python constant
carries a comment naming the C++ symbol and file, and a test asserts the Python value equals the
literal `0.10`. If someone changes the C++ default, that test does not fail — nothing can make it
fail from Python — so the comment states plainly that the two must be moved together. A silent
mismatch would mean scoring a population the bot does not actually bet, which is the exact bug
being fixed.

## Published fields

Each calibrator report gains, alongside the existing fields (which are left untouched so the
regression is visible side by side):

| Field | Meaning |
|---|---|
| `eligible_scored_contracts` | Count of contracts with at least one eligible observation |
| `brier_eligible_full` | Model Brier over eligible observations, one score per contract |
| `brier_eligible_market_mid_raw` | Market-mid Brier over the same observations |
| `min_eligible_contracts` | The floor, published so a reader can check it rather than assume it |
| `adds_value_on_bet_eligible` | `brier_eligible_full < brier_eligible_market_mid_raw` and `eligible_scored_contracts >= 100` |

For the two variant-based calibrators the same fields are published per variant inside their
existing `ablations` block, and `trusted_variant` may only name a variant that satisfies the rule
on the eligible subset. `adds_value_on_bet_eligible` is then true exactly when `trusted_variant` is
set — preserving the meaning those reports already have, on the corrected population.

`adds_value_over_market` keeps its current meaning and is not redefined. Redefining it in place
would silently change what every existing reader and stored report means; adding a field leaves the
old number auditable and makes the gap between the two the visible finding.

## Enforcement change

`KalshiBotDecision::signal_trusted()` gains one conjunct:

```cpp
return report.value("adds_value_over_market").toBool()
    && report.value("brier_full").isDouble()
    && report.value("brier_market_mid_raw").isDouble()
    && report.value("adds_value_on_bet_eligible").toBool()          // new
    && report.value("brier_eligible_full").isDouble()               // new
    && report.value("brier_eligible_market_mid_raw").isDouble();    // new
```

The existing conjuncts are kept: the change is strictly tightening, which matches the sealed gate's
tightening-only discipline.

## Error handling

Every failure path fails closed, matching the existing design.

- **Field absent** (a report written by a calibrator that predates this change): `toBool()` on a
  missing key is `false`, so the signal reads untrusted. A stale report cannot confer trust.
- **Fewer than 100 eligible contracts:** `adds_value_on_bet_eligible` is `false`. This is the
  expected state for a long time and is not an error.
- **Zero eligible contracts:** the Briers are `None`/absent, the flag is `false`.
- **Flag true but Briers missing:** refused, mirroring the existing rule that a report asserting
  value over a track record it does not carry is contradicting itself.

## Testing

Every behaviour below gets a test, and each is RED-confirmed by neutering — implement the
inverted or degenerate behaviour first, observe the test fail, then fix.

**Python** (`tests/test_calibrator_eligibility.py`, plus additions to the three calibrator tests):

1. An observation whose model/mid gap is exactly `0.10` is eligible; `0.099` is not.
2. Eligibility is symmetric — a model below the mid by the threshold is as eligible as above it.
3. A contract with no eligible observation contributes to neither Brier nor the count.
4. A contract's eligible score ignores its ineligible observations. Fixture: a contract where the
   model is wrong on eligible observations and right on ineligible ones must score badly.
5. `adds_value_on_bet_eligible` is false below the floor even when the eligible Brier wins.
6. The realistic case: a report that is `adds_value_over_market == true` on the full population and
   loses on the eligible subset publishes `adds_value_on_bet_eligible == false`.

**C++** (`tst_kalshi_bot_decision.cpp`):

7. A report trusted on the full population but with `adds_value_on_bet_eligible == false` reads
   untrusted, and `decide()` returns a `SIGNAL_UNTRUSTED` pass rather than a bid.
8. A report with the new flag true but its eligible Briers missing reads untrusted.
9. A report entirely lacking the new fields reads untrusted.

## Deployment and observation

The Python half is additive and can ship first. After the calibrators have run, the reports will
show both numbers, and the size of `eligible_scored_contracts` answers a question this design
cannot: how long the floor will actually take to reach. That number should be recorded before the
C++ half is deployed, because it determines how long the bot stays silent.

The C++ half changes behaviour on the next bot tick after deployment.

## What this does not do

It does not make the bot profitable, and no version of it could. On the contracts it bets, the
payoff ratio is 1.98x, which needs a 66.4% win rate against an observed 50.0% — a 16.4pp shortfall
that no gate tightening closes. This change stops the bot betting on a signal that has not earned
it. Finding a signal that beats the mid after fees remains the separate, unsolved problem.

Nothing here touches the sealed live gate, the kill switch, the stake caps, or the live arming
path. The bot is paper-only and the sealed gate is failing 4/4 criteria independently of this work.
