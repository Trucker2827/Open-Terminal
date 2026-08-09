# Bet-Eligible Trust Criterion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the calibrator trust flag that authorises bids measure the model on the contracts the bot actually bets, instead of on every resolved contract.

**Architecture:** A new Python module owns the eligibility predicate and its threshold. Each of the three calibrators applies that predicate inside its own existing scoring structure to build a parallel set of eligible-only Brier scores, and publishes them as new additive fields. Enforcement lands as extra conjuncts in `KalshiBotDecision::signal_trusted()`, the single C++ chokepoint that already owns trust.

**Tech Stack:** Python 3 (stdlib only, `unittest`), C++17/Qt6, CTest.

**Spec:** `openmarketterminal-qt/docs/design/2026-08-09-kalshi-bet-eligible-trust-design.md`

## Global Constraints

- Paths below are relative to `openmarketterminal-qt/`.
- Python calibrators are **stdlib only** — no new dependencies.
- Every existing published field keeps its current meaning. This change is **additive** on the Python side.
- The C++ change is **strictly tightening**: existing conjuncts stay, new ones are added.
- All failure paths **fail closed** — missing field, missing Brier, or too few contracts all mean untrusted.
- `BET_EDGE_THRESHOLD = 0.10` mirrors `KalshiBotDecision::Config::edge_threshold` (`src/services/prediction/kalshi/KalshiBotDecision.h:236`). The two must be changed together; nothing can enforce that from Python.
- The eligible-contract floor is `100`, the same value as the existing `MIN_SCORED_CONTRACTS`, applied to the eligible population.
- Run Python tests with the repo's interpreter: `python3 tests/<file>.py -v`.
- C++ tests build from the existing `build/` directory: `cmake --build build --target <target>`.

---

### Task 1: Eligibility predicate module

**Files:**
- Create: `scripts/kalshi_advise/calibrator_eligibility.py`
- Create: `tests/test_calibrator_eligibility.py`
- Modify: `tests/CMakeLists.txt:32-37` (register the new Python test beside the existing calibrator tests)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `BET_EDGE_THRESHOLD: float` (0.10)
  - `MIN_ELIGIBLE_CONTRACTS: int` (100)
  - `is_eligible(model_p: float | None, yes_mid: float | None) -> bool`
  - `eligible_pairs(rows: list[tuple[float, float]], outcome: bool) -> tuple[list, list]` — takes `(model_p, yes_mid)` per observation and returns `(model_pairs, mid_pairs)` containing only eligible observations, each as `(p, outcome)` ready for the callers' existing `brier()`.
  - `adds_value(model_scores: list, mid_scores: list) -> bool` — mean(model) < mean(mid) and `len(model_scores) >= MIN_ELIGIBLE_CONTRACTS`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_calibrator_eligibility.py`:

```python
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import calibrator_eligibility as ce


class ThresholdTest(unittest.TestCase):
    def test_threshold_mirrors_the_bot_bid_threshold(self):
        # KalshiBotDecision::Config::edge_threshold (KalshiBotDecision.h:236).
        # Nothing can enforce this from Python; the constant is pinned here so a
        # drift is at least visible in a diff.
        self.assertEqual(ce.BET_EDGE_THRESHOLD, 0.10)
        self.assertEqual(ce.MIN_ELIGIBLE_CONTRACTS, 100)

    def test_edge_exactly_at_threshold_is_eligible(self):
        self.assertTrue(ce.is_eligible(0.72, 0.62))
        self.assertFalse(ce.is_eligible(0.719, 0.62))

    def test_eligibility_is_symmetric(self):
        # A model BELOW the mid by the threshold is as bettable as one above:
        # the bot bids the other side.
        self.assertTrue(ce.is_eligible(0.52, 0.62))
        self.assertFalse(ce.is_eligible(0.521, 0.62))

    def test_missing_values_are_not_eligible(self):
        self.assertFalse(ce.is_eligible(None, 0.62))
        self.assertFalse(ce.is_eligible(0.9, None))


class EligiblePairsTest(unittest.TestCase):
    def test_ineligible_observations_are_dropped(self):
        rows = [(0.90, 0.62), (0.63, 0.62), (0.20, 0.62)]
        model, mid = ce.eligible_pairs(rows, True)
        self.assertEqual(model, [(0.90, True), (0.20, True)])
        self.assertEqual(mid, [(0.62, True), (0.62, True)])

    def test_contract_with_no_eligible_observation_yields_nothing(self):
        model, mid = ce.eligible_pairs([(0.63, 0.62), (0.60, 0.62)], False)
        self.assertEqual(model, [])
        self.assertEqual(mid, [])


class AddsValueTest(unittest.TestCase):
    def test_below_the_floor_is_false_even_when_winning(self):
        self.assertFalse(ce.adds_value([0.1] * 99, [0.4] * 99))

    def test_at_the_floor_and_winning_is_true(self):
        self.assertTrue(ce.adds_value([0.1] * 100, [0.4] * 100))

    def test_losing_at_the_floor_is_false(self):
        self.assertFalse(ce.adds_value([0.4] * 100, [0.1] * 100))

    def test_empty_is_false(self):
        self.assertFalse(ce.adds_value([], []))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_calibrator_eligibility.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'calibrator_eligibility'`

- [ ] **Step 3: Write the implementation**

Create `scripts/kalshi_advise/calibrator_eligibility.py`:

```python
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
    return abs(float(model_p) - float(yes_mid)) >= BET_EDGE_THRESHOLD


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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_calibrator_eligibility.py -v`
Expected: PASS, 9 tests

- [ ] **Step 5: Register the test with CTest**

In `tests/CMakeLists.txt`, immediately after the `test_spot_calibrator` block (line 32-34), add:

```cmake
add_test(NAME test_calibrator_eligibility
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_calibrator_eligibility.py -v)
```

- [ ] **Step 6: Verify CTest sees it**

Run: `ctest --test-dir build -N -R test_calibrator_eligibility`
Expected: lists `test_calibrator_eligibility`

- [ ] **Step 7: Commit**

```bash
git add scripts/kalshi_advise/calibrator_eligibility.py tests/test_calibrator_eligibility.py tests/CMakeLists.txt
git commit -m "feat(kalshi): eligibility predicate for the bet-eligible trust criterion"
```

---

### Task 2: spot_calibrator (single-model shape)

**Files:**
- Modify: `scripts/kalshi_advise/spot_calibrator.py` — imports, `new_state()` (~line 768), `settle_cycle` scoring loop (~lines 918-935), `build_report` (~lines 948-975)
- Test: `tests/test_spot_calibrator.py`

**Interfaces:**
- Consumes: `calibrator_eligibility.eligible_pairs`, `.adds_value`, `.MIN_ELIGIBLE_CONTRACTS`
- Produces: report fields `eligible_scored_contracts`, `brier_eligible_full`, `brier_eligible_market_mid_raw`, `min_eligible_contracts`, `adds_value_on_bet_eligible`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_spot_calibrator.py`, before the `if __name__` block:

```python
class BetEligibleTrustTest(unittest.TestCase):
    """The trust flag must be measured where the bot would actually have bet."""

    def _report(self, eligible_model, eligible_mid, n):
        state = cal.new_state()
        state["contract_scores_full"] = [0.05] * n
        state["contract_scores_market_mid_raw"] = [0.06] * n
        state["contract_scores_market_trained_logit"] = [0.07] * n
        state["contract_scores_eligible_full"] = [eligible_model] * n
        state["contract_scores_eligible_market_mid_raw"] = [eligible_mid] * n
        return cal.build_report(state, {}, 1_700_000_000_000)

    def test_publishes_the_eligible_numbers(self):
        r = self._report(0.20, 0.30, 100)
        self.assertEqual(r["eligible_scored_contracts"], 100)
        self.assertAlmostEqual(r["brier_eligible_full"], 0.20)
        self.assertAlmostEqual(r["brier_eligible_market_mid_raw"], 0.30)
        self.assertEqual(r["min_eligible_contracts"], 100)

    def test_trusted_on_the_full_population_but_losing_where_it_bets(self):
        # The live shape: model wins the easy far-from-strike population and
        # loses the near-money one it actually bets.
        r = self._report(0.2576, 0.2130, 100)
        self.assertTrue(r["adds_value_over_market"])
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_winning_where_it_bets_at_the_floor(self):
        r = self._report(0.2130, 0.2576, 100)
        self.assertTrue(r["adds_value_on_bet_eligible"])

    def test_below_the_eligible_floor_is_false_even_when_winning(self):
        r = self._report(0.2130, 0.2576, 99)
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_settle_cycle_scores_only_eligible_observations(self):
        # Two observations: one where the model is far from the mid (eligible)
        # and one where it hugs the mid (not). Only the first may be scored.
        state = cal.new_state()
        rows = [(0.95, 0.60), (0.61, 0.60)]
        model_pairs, mid_pairs = cal.ce.eligible_pairs(rows, True)
        self.assertEqual(len(model_pairs), 1)
        self.assertEqual(model_pairs[0][0], 0.95)
        self.assertEqual(mid_pairs[0][0], 0.60)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_spot_calibrator.py BetEligibleTrustTest -v`
Expected: FAIL — `KeyError: 'eligible_scored_contracts'` (and `AttributeError: module 'spot_calibrator' has no attribute 'ce'`)

- [ ] **Step 3: Add the import and state lists**

Near the other imports at the top of `scripts/kalshi_advise/spot_calibrator.py`:

```python
import calibrator_eligibility as ce
```

In `new_state()`, beside the existing score lists:

```python
            "contract_scores_full": [],
            "contract_scores_market_trained_logit": [],
            "contract_scores_market_mid_raw": [],
            # Same contracts, scored over ONLY the observations where the model's
            # edge over the mid reached the bot's bid threshold -- the population
            # the trust flag is actually used to authorise. See
            # calibrator_eligibility and docs/design/2026-08-09-kalshi-bet-eligible-trust-design.md.
            "contract_scores_eligible_full": [],
            "contract_scores_eligible_market_mid_raw": [],
```

- [ ] **Step 4: Score the eligible subset in settle_cycle**

In the scoring loop, immediately after the three existing `state["contract_scores_*"].append(...)` calls:

```python
        eligible_model, eligible_mid = ce.eligible_pairs(
            [(full.predict(f), f["yes_mid"]) for f in observations], outcome)
        if eligible_model:
            state["contract_scores_eligible_full"].append(brier(eligible_model))
            state["contract_scores_eligible_market_mid_raw"].append(brier(eligible_mid))
```

Add both new keys to the window-trim tuple so they stay bounded like the rest:

```python
    for key in ("contract_scores_full", "contract_scores_market_trained_logit",
                "contract_scores_market_mid_raw", "resolved_record",
                "contract_scores_eligible_full",
                "contract_scores_eligible_market_mid_raw"):
        state[key] = state[key][-SCORED_CONTRACT_WINDOW:]
```

- [ ] **Step 5: Publish the fields in build_report**

Near the top of `build_report`, beside the existing `scored`/`b_full` lines:

```python
    eligible = state.get("contract_scores_eligible_full") or []
    eligible_mid = state.get("contract_scores_eligible_market_mid_raw") or []
```

Add to the returned dict, after `beats_trained_logit_baseline`:

```python
        "eligible_scored_contracts": len(eligible),
        "brier_eligible_full": mean_or_none(eligible),
        "brier_eligible_market_mid_raw": mean_or_none(eligible_mid),
        "min_eligible_contracts": ce.MIN_ELIGIBLE_CONTRACTS,
        "adds_value_on_bet_eligible": ce.adds_value(eligible, eligible_mid),
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `python3 tests/test_spot_calibrator.py -v`
Expected: PASS, all pre-existing tests plus the 5 new ones

- [ ] **Step 7: Commit**

```bash
git add scripts/kalshi_advise/spot_calibrator.py tests/test_spot_calibrator.py
git commit -m "feat(kalshi): score spot_calibrator trust on the bet-eligible subset"
```

---

### Task 3: kxbtc15m_calibrator (variant shape)

**Files:**
- Modify: `scripts/kalshi_advise/kxbtc15m_calibrator.py` — imports, `new_state()` (~line 389), the `setdefault` block (~line 414), `ablation_scoreboard`/`select_trusted_variant` (~line 644), the scoring loop (~lines 861-883), `build_report`
- Test: `tests/test_kxbtc15m_calibrator.py`

**Interfaces:**
- Consumes: `calibrator_eligibility.eligible_pairs`, `.MIN_ELIGIBLE_CONTRACTS`; the existing `oif.paired_ablation_scoreboard` and `oif.select_best_trusted`
- Produces: the same five report fields as Task 2, plus `eligible_ablations` and a `trusted_variant` that may only be set from the eligible board

This calibrator scores five predictors (`physics` plus four ablation variants) against the mid and picks the best that beats it. The change reuses that machinery exactly: build a parallel set of eligible-only score lists with the same keys, run the same scoreboard over them, and select `trusted_variant` from the eligible board.

Eligibility is evaluated **per predictor** — a contract can be eligible for one variant and not another, which is correct, because the bot would only have bet the variant whose edge cleared the threshold.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_kxbtc15m_calibrator.py`, before the `if __name__` block:

```python
class BetEligibleTrustTest(unittest.TestCase):
    VARIANTS = ("physics", "physics_veto_on_conflict", "physics_confirm_only",
                "physics_brti_avg60", "physics_vol_regime_confirm")

    def _state(self, eligible_model, eligible_mid, n):
        state = cal.new_state()
        state["contract_scores_full"] = [0.05] * n
        state["contract_scores_market_mid_raw"] = [0.06] * n
        for key in ("physics_veto_on_conflict", "physics_confirm_only",
                    "physics_brti_avg60", "physics_vol_regime_confirm"):
            state[f"contract_scores_{key}"] = [0.05] * n
            state[f"contract_scores_eligible_{key}"] = [eligible_model] * n
        state["contract_scores_eligible_full"] = [eligible_model] * n
        state["contract_scores_eligible_market_mid_raw"] = [eligible_mid] * n
        return state

    def test_losing_where_it_bets_is_untrusted_on_the_new_flag(self):
        # Wins the easy full population, loses the population it bets.
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_winning_where_it_bets_can_earn_the_new_flag(self):
        state = self._state(0.2130, 0.2576, 100)
        self.assertIsNotNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertTrue(r["adds_value_on_bet_eligible"])

    def test_below_the_eligible_floor_is_untrusted(self):
        state = self._state(0.2130, 0.2576, 99)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))

    def test_live_predictor_selection_is_untouched(self):
        # select_trusted_variant also picks live_p (the PUBLISHED probability).
        # This change must not move it: it gates trust, not prediction.
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNotNone(cal.select_trusted_variant(state))

    def test_publishes_the_eligible_numbers(self):
        r = cal.build_report(self._state(0.21, 0.26, 100), {}, 1_700_000_000_000)
        self.assertEqual(r["eligible_scored_contracts"], 100)
        self.assertAlmostEqual(r["brier_eligible_full"], 0.21)
        self.assertAlmostEqual(r["brier_eligible_market_mid_raw"], 0.26)
        self.assertEqual(r["min_eligible_contracts"], 100)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_kxbtc15m_calibrator.py BetEligibleTrustTest -v`
Expected: FAIL — `AttributeError: module 'kxbtc15m_calibrator' has no attribute 'select_trusted_variant_eligible'`

- [ ] **Step 3: Add the import, state lists and setdefaults**

At the top of `scripts/kalshi_advise/kxbtc15m_calibrator.py`:

```python
import calibrator_eligibility as ce
```

In `new_state()`, beside the existing `contract_scores_*` keys:

```python
        "contract_scores_eligible_full": [],
        "contract_scores_eligible_market_mid_raw": [],
        "contract_scores_eligible_physics_veto_on_conflict": [],
        "contract_scores_eligible_physics_confirm_only": [],
        "contract_scores_eligible_physics_brti_avg60": [],
        "contract_scores_eligible_physics_vol_regime_confirm": [],
```

And the matching `setdefault` calls beside the existing ones (so a state file written before this change loads without a `KeyError`):

```python
    state.setdefault("contract_scores_eligible_full", [])
    state.setdefault("contract_scores_eligible_market_mid_raw", [])
    state.setdefault("contract_scores_eligible_physics_veto_on_conflict", [])
    state.setdefault("contract_scores_eligible_physics_confirm_only", [])
    state.setdefault("contract_scores_eligible_physics_brti_avg60", [])
    state.setdefault("contract_scores_eligible_physics_vol_regime_confirm", [])
```

- [ ] **Step 4: Add the eligible scoreboard and re-point variant selection**

Beside the existing `ablation_scoreboard`:

```python
def eligible_ablation_scoreboard(state):
    """Per-variant Brier vs mid over the BET-ELIGIBLE observations only."""
    return oif.paired_ablation_scoreboard(
        {
            "physics": state.get("contract_scores_eligible_full") or [],
            "physics_veto_on_conflict":
                state.get("contract_scores_eligible_physics_veto_on_conflict") or [],
            "physics_confirm_only":
                state.get("contract_scores_eligible_physics_confirm_only") or [],
            "physics_brti_avg60":
                state.get("contract_scores_eligible_physics_brti_avg60") or [],
            "physics_vol_regime_confirm":
                state.get("contract_scores_eligible_physics_vol_regime_confirm") or [],
        },
        state.get("contract_scores_eligible_market_mid_raw") or [],
        ce.MIN_ELIGIBLE_CONTRACTS,
    )
```

Add a SEPARATE selector beside the existing one. Do **not** re-point
`select_trusted_variant`: it also chooses `live_p`, the published probability
(line ~646, `live_p = obs["p_ablations"][trusted_variant]`). Moving it would
change which model's forecast ships, which is a prediction change, not a trust
change, and is out of scope.

```python
def select_trusted_variant_eligible(state):
    """Best ablation that beats mid ON THE BET-ELIGIBLE SUBSET at >=100
    contracts; else None (fail-closed).

    Deliberately separate from select_trusted_variant: that one also selects
    live_p, the published probability. This one gates trust only.
    """
    board, _b_mid = eligible_ablation_scoreboard(state)
    return oif.select_best_trusted(board, ABLATION_KEYS)
```

- [ ] **Step 5: Score the eligible subset in the settle loop**

In the scoring loop, after the four existing per-variant `.append(brier(...))` calls, add:

```python
        eligible_rows = [(obs["p_model"], obs["yes_mid"]) for obs in observations]
        eligible_model, eligible_mid = ce.eligible_pairs(eligible_rows, outcome)
        if eligible_model:
            state["contract_scores_eligible_full"].append(brier(eligible_model))
            state["contract_scores_eligible_market_mid_raw"].append(brier(eligible_mid))
        for key in ("physics_veto_on_conflict", "physics_confirm_only",
                    "physics_brti_avg60", "physics_vol_regime_confirm"):
            rows = [((obs.get("p_ablations") or {}).get(key, obs["p_model"]), obs["yes_mid"])
                    for obs in observations]
            v_model, _v_mid = ce.eligible_pairs(rows, outcome)
            if v_model:
                state[f"contract_scores_eligible_{key}"].append(brier(v_model))
```

Add all six new keys to the window-trim tuple beside the existing ones.

- [ ] **Step 6: Publish the fields in build_report**

In `build_report`, beside the existing `ablations, b_mid = ablation_scoreboard(state)`:

```python
    eligible_ablations, b_eligible_mid = eligible_ablation_scoreboard(state)
    eligible = state.get("contract_scores_eligible_full") or []
    trusted_variant_eligible = select_trusted_variant_eligible(state)
```

and add to the returned dict:

```python
        "eligible_ablations": eligible_ablations,
        "eligible_scored_contracts": len(eligible),
        "brier_eligible_full": mean_or_none(eligible),
        "brier_eligible_market_mid_raw": b_eligible_mid,
        "min_eligible_contracts": ce.MIN_ELIGIBLE_CONTRACTS,
        "trusted_variant_eligible": trusted_variant_eligible,
        "adds_value_on_bet_eligible": trusted_variant_eligible is not None,
```

`adds_value_over_market`, `trusted_variant` and `live_p` are all left exactly as
they are. The tightening comes from Task 5, where C++ requires BOTH flags. This
keeps the Python change additive, as the spec requires, and keeps the published
forecast unchanged.

- [ ] **Step 7: Run tests to verify they pass**

Run: `python3 tests/test_kxbtc15m_calibrator.py -v`
Expected: PASS, all pre-existing tests plus the 4 new ones

- [ ] **Step 8: Commit**

```bash
git add scripts/kalshi_advise/kxbtc15m_calibrator.py tests/test_kxbtc15m_calibrator.py
git commit -m "feat(kalshi): score kxbtc15m variant trust on the bet-eligible subset"
```

---

### Task 4: commodities_15m_calibrator (variant shape, two variants)

**Files:**
- Modify: `scripts/kalshi_advise/commodities_15m_calibrator.py` — imports, `new_state()` (~line 512), the `setdefault` block (~line 537), `ablation_scoreboard` (~line 551), `select_trusted_variant`, the scoring loop (~line 754), `build_report` (~line 850)
- Test: `tests/test_commodities_15m_calibrator.py`

**Interfaces:**
- Consumes: `calibrator_eligibility.eligible_pairs`, `.MIN_ELIGIBLE_CONTRACTS`; the existing `oif.paired_ablation_scoreboard` and `oif.select_best_trusted`.
- Produces: same five fields; `trusted_variant` selected from the eligible board.

Identical in shape to Task 3 with two variants instead of four: `physics_tape_confirm_near_close` and `physics_vol_regime_confirm`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_commodities_15m_calibrator.py`, before the `if __name__` block:

```python
class BetEligibleTrustTest(unittest.TestCase):
    def _state(self, eligible_model, eligible_mid, n):
        state = cal.new_state()
        state["contract_scores_full"] = [0.05] * n
        state["contract_scores_market_mid_raw"] = [0.06] * n
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            state[f"contract_scores_{key}"] = [0.05] * n
            state[f"contract_scores_eligible_{key}"] = [eligible_model] * n
        state["contract_scores_eligible_full"] = [eligible_model] * n
        state["contract_scores_eligible_market_mid_raw"] = [eligible_mid] * n
        return state

    def test_losing_where_it_bets_is_untrusted(self):
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_winning_where_it_bets_can_earn_trust(self):
        state = self._state(0.2130, 0.2576, 100)
        self.assertIsNotNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertTrue(r["adds_value_on_bet_eligible"])

    def test_below_the_eligible_floor_is_untrusted(self):
        self.assertIsNone(cal.select_trusted_variant_eligible(self._state(0.2130, 0.2576, 99)))

    def test_publishes_the_eligible_numbers(self):
        r = cal.build_report(self._state(0.21, 0.26, 100), {}, 1_700_000_000_000)
        self.assertEqual(r["eligible_scored_contracts"], 100)
        self.assertAlmostEqual(r["brier_eligible_full"], 0.21)
        self.assertEqual(r["min_eligible_contracts"], 100)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_commodities_15m_calibrator.py BetEligibleTrustTest -v`
Expected: FAIL — `AttributeError: module 'commodities_15m_calibrator' has no attribute 'select_trusted_variant_eligible'`

- [ ] **Step 3: Add the import, state lists and setdefaults**

At the top of `scripts/kalshi_advise/commodities_15m_calibrator.py`:

```python
import calibrator_eligibility as ce
```

In `new_state()`, beside the existing `contract_scores_*` keys:

```python
        "contract_scores_eligible_full": [],
        "contract_scores_eligible_market_mid_raw": [],
        "contract_scores_eligible_physics_tape_confirm_near_close": [],
        "contract_scores_eligible_physics_vol_regime_confirm": [],
```

And the matching `setdefault` calls beside the existing ones, so a state file
written before this change loads without a `KeyError`:

```python
    state.setdefault("contract_scores_eligible_full", [])
    state.setdefault("contract_scores_eligible_market_mid_raw", [])
    state.setdefault("contract_scores_eligible_physics_tape_confirm_near_close", [])
    state.setdefault("contract_scores_eligible_physics_vol_regime_confirm", [])
```

- [ ] **Step 4: Add the eligible scoreboard and a separate selector**

Beside the existing `ablation_scoreboard`:

```python
def eligible_ablation_scoreboard(state):
    """Per-variant Brier vs mid over the BET-ELIGIBLE observations only."""
    return oif.paired_ablation_scoreboard(
        {
            "physics": state.get("contract_scores_eligible_full") or [],
            "physics_tape_confirm_near_close":
                state.get("contract_scores_eligible_physics_tape_confirm_near_close") or [],
            "physics_vol_regime_confirm":
                state.get("contract_scores_eligible_physics_vol_regime_confirm") or [],
        },
        state.get("contract_scores_eligible_market_mid_raw") or [],
        ce.MIN_ELIGIBLE_CONTRACTS,
    )


def select_trusted_variant_eligible(state):
    """Best ablation that beats mid ON THE BET-ELIGIBLE SUBSET at >=100
    contracts; else None (fail-closed).

    Deliberately separate from select_trusted_variant, which also selects the
    published probability. This one gates trust only.
    """
    board, _b_mid = eligible_ablation_scoreboard(state)
    return oif.select_best_trusted(board, ABLATION_KEYS)
```

Leave `select_trusted_variant` untouched.

- [ ] **Step 5: Score the eligible subset in the settle loop**

In the scoring loop, after the existing per-variant `.append(brier(...))` calls:

```python
        eligible_rows = [(obs["p_model"], obs["yes_mid"]) for obs in observations]
        eligible_model, eligible_mid = ce.eligible_pairs(eligible_rows, outcome)
        if eligible_model:
            state["contract_scores_eligible_full"].append(brier(eligible_model))
            state["contract_scores_eligible_market_mid_raw"].append(brier(eligible_mid))
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            rows = [((obs.get("p_ablations") or {}).get(key, obs["p_model"]), obs["yes_mid"])
                    for obs in observations]
            v_model, _v_mid = ce.eligible_pairs(rows, outcome)
            if v_model:
                state[f"contract_scores_eligible_{key}"].append(brier(v_model))
```

Add all four new keys to the window-trim tuple beside the existing ones.

- [ ] **Step 6: Publish the fields in build_report**

Beside the existing `ablations, b_mid = ablation_scoreboard(state)`:

```python
    eligible_ablations, b_eligible_mid = eligible_ablation_scoreboard(state)
    eligible = state.get("contract_scores_eligible_full") or []
    trusted_variant_eligible = select_trusted_variant_eligible(state)
```

and add to the returned dict:

```python
        "eligible_ablations": eligible_ablations,
        "eligible_scored_contracts": len(eligible),
        "brier_eligible_full": mean_or_none(eligible),
        "brier_eligible_market_mid_raw": b_eligible_mid,
        "min_eligible_contracts": ce.MIN_ELIGIBLE_CONTRACTS,
        "trusted_variant_eligible": trusted_variant_eligible,
        "adds_value_on_bet_eligible": trusted_variant_eligible is not None,
```

`adds_value_over_market`, `trusted_variant` and the published probability are
left exactly as they are.

- [ ] **Step 7: Run tests to verify they pass**

Run: `python3 tests/test_commodities_15m_calibrator.py -v`
Expected: PASS, all pre-existing tests plus the 4 new ones

- [ ] **Step 8: Commit**

```bash
git add scripts/kalshi_advise/commodities_15m_calibrator.py tests/test_commodities_15m_calibrator.py
git commit -m "feat(kalshi): score commodities 15m variant trust on the bet-eligible subset"
```

---

### Task 5: Enforce it in the C++ chokepoint

**Files:**
- Modify: `src/services/prediction/kalshi/KalshiBotDecision.cpp:88-95` (`signal_trusted`)
- Modify: `src/services/prediction/kalshi/KalshiBotDecision.h:36-50` (the doc comment describing rule 4)
- Test: `tests/tst_kalshi_bot_decision.cpp`

**Interfaces:**
- Consumes: the report fields produced by Tasks 2-4.
- Produces: no new symbols. `signal_trusted()` keeps its signature.

- [ ] **Step 1: Write the failing test**

Append to `tests/tst_kalshi_bot_decision.cpp`, inside the existing test class's `private slots:` section:

```cpp
    // Regression: adds_value_over_market is measured over EVERY resolved
    // contract, a population dominated by far-from-strike contracts the bot
    // never bets. Trust must additionally require the model to beat the mid on
    // the contracts whose edge cleared the bid threshold.
    void signal_untrusted_when_it_loses_where_it_bets() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), false},
            {QStringLiteral("brier_eligible_full"), 0.2576},
            {QStringLiteral("brier_eligible_market_mid_raw"), 0.2130}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }

    void signal_trusted_when_it_wins_where_it_bets() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), true},
            {QStringLiteral("brier_eligible_full"), 0.2130},
            {QStringLiteral("brier_eligible_market_mid_raw"), 0.2576}};
        QVERIFY(KalshiBotDecision::signal_trusted(report));
    }

    // A report predating this change cannot confer trust: the same fail-closed
    // rule the existing conjuncts apply to a missing Brier.
    void signal_untrusted_when_the_eligible_fields_are_absent() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }

    // Claiming value over a track record it does not carry is self-contradiction.
    void signal_untrusted_when_eligible_flag_lacks_its_briers() {
        QJsonObject report{
            {QStringLiteral("adds_value_over_market"), true},
            {QStringLiteral("brier_full"), 0.06767},
            {QStringLiteral("brier_market_mid_raw"), 0.06897},
            {QStringLiteral("adds_value_on_bet_eligible"), true}};
        QVERIFY(!KalshiBotDecision::signal_trusted(report));
    }
```

- [ ] **Step 2: Build and run to verify it fails**

Run:
```bash
cmake --build build --target tst_kalshi_bot_decision && ./build/tests/tst_kalshi_bot_decision
```
Expected: FAIL — `signal_untrusted_when_it_loses_where_it_bets` and `signal_untrusted_when_the_eligible_fields_are_absent` both pass trust through, because the new conjuncts do not exist yet.

- [ ] **Step 3: Add the conjuncts**

Replace the body of `KalshiBotDecision::signal_trusted` in `src/services/prediction/kalshi/KalshiBotDecision.cpp`:

```cpp
bool KalshiBotDecision::signal_trusted(const QJsonObject& report) {
    // The claim, and the measurement it is a claim about. `track_record()`
    // already calls a report without both Briers unavailable; a report that
    // asserts value over a track record it does not carry is contradicting
    // itself, and this fails closed on it rather than believing the flag.
    //
    // `adds_value_over_market` is measured over EVERY resolved contract, a
    // population dominated by far-from-strike contracts where both the model
    // and the market are nearly always right. The bot does not bet that
    // population -- it bets where its edge over the mid clears
    // `edge_threshold`, and there the market has been winning. So trust
    // additionally requires the model to beat the mid on the BET-ELIGIBLE
    // subset. A report predating that field cannot confer trust.
    return report.value(QStringLiteral("adds_value_over_market")).toBool() &&
           report.value(QStringLiteral("brier_full")).isDouble() &&
           report.value(QStringLiteral("brier_market_mid_raw")).isDouble() &&
           report.value(QStringLiteral("adds_value_on_bet_eligible")).toBool() &&
           report.value(QStringLiteral("brier_eligible_full")).isDouble() &&
           report.value(QStringLiteral("brier_eligible_market_mid_raw")).isDouble();
}
```

- [ ] **Step 4: Update the header contract**

In `src/services/prediction/kalshi/KalshiBotDecision.h`, in the rule-4 paragraph of the class doc comment, after the sentence ending "`spot_calibrator.MIN_SCORED_CONTRACTS`, issue #171)", insert:

```
///      AND once that same comparison holds on the BET-ELIGIBLE subset — the
///      contracts whose model-vs-mid edge reached `edge_threshold`, which is
///      the population a bid is actually drawn from. The full-population flag
///      alone was measured where the bot does not bet.
```

- [ ] **Step 5: Build and run to verify it passes**

Run:
```bash
cmake --build build --target tst_kalshi_bot_decision && ./build/tests/tst_kalshi_bot_decision
```
Expected: PASS, all pre-existing tests plus the 4 new ones

- [ ] **Step 6: Run the neighbouring suites**

Run:
```bash
cmake --build build --target tst_serve_command tst_kalshi_bot_gate tst_kalshi_bot_funnel
./build/tests/tst_serve_command && ./build/tests/tst_kalshi_bot_gate && ./build/tests/tst_kalshi_bot_funnel
ctest --test-dir build -R "e2e_kalshi_bot_"
```
Expected: all PASS. The e2e bot tests seed `calibrator.json` fixtures that lack the new fields, so if any now fail with `SIGNAL_UNTRUSTED` at their control step, extend those fixtures with `adds_value_on_bet_eligible`, `brier_eligible_full` and `brier_eligible_market_mid_raw` — the same last-mile fixture update as `5f108ce9`.

- [ ] **Step 7: Commit**

```bash
git add src/services/prediction/kalshi/KalshiBotDecision.cpp \
        src/services/prediction/kalshi/KalshiBotDecision.h \
        tests/tst_kalshi_bot_decision.cpp
git commit -m "fix(kalshi): require trust on the bet-eligible subset before bidding"
```

---

## After the plan

Do **not** deploy the C++ half without first running the calibrators and recording `eligible_scored_contracts`. That number answers the one question the design could not: how long the bot stays silent before it could possibly earn trust again. Record it in `MemoryMD/kalshi-bot-supervision.md`.

Expected outcome after deployment: `signal_trusted` goes false, `SIGNAL_UNTRUSTED` passes replace bids, and the bot stops trading until the model beats the mid where it acts. That is the criterion working.
