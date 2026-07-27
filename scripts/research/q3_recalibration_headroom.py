#!/usr/bin/env python3
"""Q3 of the Kalshi edge autopsy (issue #169): out-of-sample recalibration gain.

Fits a correction to the calibrator's probability on past contracts and scores
it on future ones. Two correctors, both dependency-free and both from the arena
overlay pattern of PR #110: a 2-parameter Platt scaling and a pool-adjacent-
violators isotonic fit.

THE SPLIT IS BY CONTRACT CLOSE TIME, NEVER BY ROW. A contract contributes up to
~56 forecast rows that all share one outcome; splitting rows at random would put
the same outcome on both sides of the boundary and manufacture out-of-sample
"improvement" out of nothing. Every split here cuts the CONTRACT list ordered by
close time, so a contract is wholly in train or wholly in test and the test set
is strictly in the future of the training set.

Three evaluations, increasingly strict:
  half/half     fit on the earlier half of contracts, score the later half
  walk-forward  expanding window over sequential folds — the honest estimate
  in-sample     fit and score the same rows, printed ONLY as the overfitting
                yardstick the other two should be compared against

The market mid is carried through every table. A recalibration that improves the
calibrator but still trails the raw mid has found no edge, only a smaller loss,
and the report is required to say so.

Read-only. Prints JSON to stdout; writes nothing.

  python3 scripts/research/q3_recalibration_headroom.py
"""
import sys

import kalshi_edge_common as common
import forecast_history

MIN_TRAIN_CONTRACTS = 20
MIN_TEST_CONTRACTS = 10
WALK_FORWARD_FOLDS = 5


def rows_of(contracts):
    return [row for contract in contracts for row in contract["rows"]]


def pairs(rows, field="calibrated_p"):
    return [(row[field], row["outcome"]) for row in rows]


def evaluate(train_contracts, test_contracts):
    """Fit both correctors on train, score everything on test."""
    train_rows = rows_of(train_contracts)
    test_rows = rows_of(test_contracts)
    if not train_rows or not test_rows:
        return None
    train_pairs = pairs(train_rows)
    test_pairs = pairs(test_rows)

    a, b = common.platt_fit(train_pairs)
    blocks = common.isotonic_fit(train_pairs)

    raw = common.brier(test_pairs)
    platt = common.brier([(common.platt_apply(a, b, p), y) for p, y in test_pairs])
    isotonic = common.brier([(common.isotonic_apply(blocks, p), y)
                             for p, y in test_pairs])
    market = common.brier(pairs(test_rows, "market_mid"))
    return {
        "train_contracts": len(train_contracts), "train_rows": len(train_rows),
        "test_contracts": len(test_contracts), "test_rows": len(test_rows),
        "test_span_utc": [common.iso(test_contracts[0]["close_ms"]),
                          common.iso(test_contracts[-1]["close_ms"])],
        "platt_a": a, "platt_b": b,
        "isotonic_blocks": len(blocks),
        "brier_raw_calibrated": raw,
        "brier_platt": platt,
        "brier_isotonic": isotonic,
        "brier_market_mid": market,
        "platt_gain_vs_raw": raw - platt if raw is not None else None,
        "isotonic_gain_vs_raw": raw - isotonic if raw is not None else None,
        "best_corrected_vs_market": (min(platt, isotonic) - market
                                     if market is not None else None),
        "any_corrector_beats_market": (market is not None
                                       and min(platt, isotonic) < market),
    }


def walk_forward(contracts, folds=WALK_FORWARD_FOLDS):
    """Expanding-window folds over contracts ordered by close time."""
    total = len(contracts)
    if total < MIN_TRAIN_CONTRACTS + MIN_TEST_CONTRACTS:
        return {"folds": [],
                "note": ("insufficient contracts for a walk-forward: need "
                         f"{MIN_TRAIN_CONTRACTS + MIN_TEST_CONTRACTS}, have {total}")}
    step = (total - MIN_TRAIN_CONTRACTS) // folds
    if step < MIN_TEST_CONTRACTS:
        folds = max(1, (total - MIN_TRAIN_CONTRACTS) // MIN_TEST_CONTRACTS)
        step = (total - MIN_TRAIN_CONTRACTS) // folds
    results = []
    for index in range(folds):
        cut = MIN_TRAIN_CONTRACTS + index * step
        end = cut + step if index < folds - 1 else total
        fold = evaluate(contracts[:cut], contracts[cut:end])
        if fold:
            fold["fold"] = index + 1
            results.append(fold)
    aggregate = None
    if results:
        weight = sum(f["test_rows"] for f in results)
        def pooled(key):
            return sum(f[key] * f["test_rows"] for f in results) / weight
        aggregate = {
            "folds": len(results), "pooled_test_rows": weight,
            "pooled_test_contracts": sum(f["test_contracts"] for f in results),
            "brier_raw_calibrated": pooled("brier_raw_calibrated"),
            "brier_platt": pooled("brier_platt"),
            "brier_isotonic": pooled("brier_isotonic"),
            "brier_market_mid": pooled("brier_market_mid"),
        }
        aggregate["platt_gain_vs_raw"] = (aggregate["brier_raw_calibrated"]
                                          - aggregate["brier_platt"])
        aggregate["isotonic_gain_vs_raw"] = (aggregate["brier_raw_calibrated"]
                                             - aggregate["brier_isotonic"])
        aggregate["best_corrected_vs_market"] = (
            min(aggregate["brier_platt"], aggregate["brier_isotonic"])
            - aggregate["brier_market_mid"])
        aggregate["any_corrector_beats_market"] = (
            aggregate["best_corrected_vs_market"] < 0)
    return {"folds": results, "aggregate": aggregate}


def in_sample(contracts):
    """The overfitting yardstick: fit and score the same rows."""
    rows = rows_of(contracts)
    if not rows:
        return None
    all_pairs = pairs(rows)
    a, b = common.platt_fit(all_pairs)
    blocks = common.isotonic_fit(all_pairs)
    return {
        "rows": len(rows), "contracts": len(contracts),
        "brier_raw_calibrated": common.brier(all_pairs),
        "brier_platt": common.brier([(common.platt_apply(a, b, p), y)
                                     for p, y in all_pairs]),
        "brier_isotonic": common.brier([(common.isotonic_apply(blocks, p), y)
                                        for p, y in all_pairs]),
        "brier_market_mid": common.brier(pairs(rows, "market_mid")),
        "warning": ("in-sample by construction — an isotonic fit can drive this "
                    "arbitrarily low and it is NOT evidence of available edge"),
    }


def main():
    history = forecast_history.build()
    contracts = sorted(history["contracts"], key=lambda c: c["close_ms"])
    if len(contracts) < MIN_TRAIN_CONTRACTS + MIN_TEST_CONTRACTS:
        common.emit({"as_of_utc": common.as_of(), "question": "Q3",
                     "verdict": "INSUFFICIENT DATA",
                     "contracts_available": len(contracts),
                     "contracts_required": MIN_TRAIN_CONTRACTS + MIN_TEST_CONTRACTS,
                     "audit": history["audit"]})
        return 0

    midpoint = len(contracts) // 2
    recorded_only = [c for c in contracts if c["outcome_source"] == "recorded"]

    common.emit({
        "as_of_utc": common.as_of(),
        "question": "Q3 — recalibration headroom",
        "command": "python3 scripts/research/q3_recalibration_headroom.py",
        "audit": history["audit"],
        "split_policy": ("contracts ordered by close time; a contract is wholly "
                         "in train or wholly in test, never split by row"),
        "contract_span_utc": [common.iso(contracts[0]["close_ms"]),
                              common.iso(contracts[-1]["close_ms"])],
        "half_and_half": evaluate(contracts[:midpoint], contracts[midpoint:]),
        "walk_forward": walk_forward(contracts),
        "in_sample_yardstick": in_sample(contracts),
        "recorded_settlements_only": {
            "contracts": len(recorded_only),
            "half_and_half": (evaluate(recorded_only[:len(recorded_only) // 2],
                                       recorded_only[len(recorded_only) // 2:])
                              if len(recorded_only) >= MIN_TRAIN_CONTRACTS
                              + MIN_TEST_CONTRACTS else None),
        },
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
