#!/usr/bin/env python3
"""Where the threshold model loses to mid on *bet-eligible* contracts.

This is NOT bid postmortem (`bot_bid_postmortem.py`). Bid postmortem joins
paper fills to settlements. This walk-forwards `spot-calibrator-state.json`'s
`resolved_record` the same way settle_cycle scores, keeps only contracts with
at least one observation where |model_p − yes_mid| ≥ edge threshold, and
summarizes when model Brier is worse than raw mid — the population that must
improve before `adds_value_on_bet_eligible` can flip.

  python3 scripts/kalshi_advise/eligible_loss_autopsy.py
  python3 scripts/kalshi_advise/eligible_loss_autopsy.py --json
  openterminalcli kalshi bot eligible-autopsy
"""
from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
from openterminal_paths import evidence_dir  # noqa: E402

sys.path.insert(0, str(_HERE))
import calibrator_eligibility as ce  # noqa: E402
import spot_calibrator as sc  # noqa: E402

STATE_NAME = "spot-calibrator-state.json"


def _mean(xs: Sequence[float]) -> Optional[float]:
    if not xs:
        return None
    return sum(xs) / len(xs)


def _edge_bucket(edge: float) -> str:
    if edge < 0.12:
        return "0.10–0.12"
    if edge < 0.15:
        return "0.12–0.15"
    if edge < 0.20:
        return "0.15–0.20"
    return "≥0.20"


def _minutes_bucket(minutes: float) -> str:
    if minutes < 15:
        return "<15m"
    if minutes < 30:
        return "15–30m"
    if minutes < 60:
        return "30–60m"
    return "≥60m"


def walk_eligible(
    resolved_record: Sequence[dict],
) -> List[Dict[str, Any]]:
    """Walk-forward score; one row per eligible contract (OOS before train)."""
    full = sc.OnlineLogit(sc.FULL_FEATURES)
    rows: List[Dict[str, Any]] = []
    for index, record in enumerate(resolved_record):
        observations = record.get("observations") or []
        outcome = bool(record.get("outcome"))
        if not observations:
            continue
        preds = [(full.predict(f), float(f.get("yes_mid") or 0.0), f) for f in observations]
        model_pairs, mid_pairs = ce.eligible_pairs(
            [(p, mid) for p, mid, _ in preds], outcome
        )
        for f in observations:
            full.update(f, outcome, l2=sc.L2)
        if not model_pairs:
            continue
        model_brier = sc.brier(model_pairs)
        mid_brier = sc.brier(mid_pairs)
        edges = [abs(p - mid) for p, mid, _ in preds if ce.is_eligible(p, mid)]
        minutes = []
        for p, mid, f in preds:
            if not ce.is_eligible(p, mid):
                continue
            sqrt_m = float(f.get("sqrt_minutes_left") or 0.0)
            minutes.append(sqrt_m * sqrt_m if sqrt_m >= 0.0 else 0.0)
        # Thesis side from mean eligible model vs mid (bot bids that side).
        elig_ps = [p for p, mid, _ in preds if ce.is_eligible(p, mid)]
        elig_mids = [mid for p, mid, _ in preds if ce.is_eligible(p, mid)]
        mean_p = _mean(elig_ps) or 0.0
        mean_mid = _mean(elig_mids) or 0.0
        thesis = "YES" if mean_p >= mean_mid else "NO"
        thesis_correct = (thesis == "YES" and outcome) or (thesis == "NO" and not outcome)
        # What a ZERO-information thesis scores if the mid is calibrated: siding
        # YES on a contract the market prices at 0.70 is right 70% of the time.
        # Without this, the hit rate alone reads as skill when it is base rate.
        thesis_baseline = mean_mid if thesis == "YES" else (1.0 - mean_mid)
        mean_edge = _mean(edges) or 0.0
        mean_minutes = _mean(minutes) or 0.0
        rows.append(
            {
                "index": index,
                "outcome": outcome,
                "eligible_obs": len(model_pairs),
                "model_brier": model_brier,
                "mid_brier": mid_brier,
                "model_loses": model_brier > mid_brier + 1e-12,
                "brier_delta": model_brier - mid_brier,
                "mean_edge": mean_edge,
                "mean_minutes_left": mean_minutes,
                "thesis": thesis,
                "thesis_correct": thesis_correct,
                "thesis_baseline": thesis_baseline,
                "edge_bucket": _edge_bucket(mean_edge),
                "minutes_bucket": _minutes_bucket(mean_minutes),
            }
        )
    return rows


def summarize(eligible_rows: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    losses = [r for r in eligible_rows if r["model_loses"]]
    wins = [r for r in eligible_rows if not r["model_loses"]]
    by_edge: Dict[str, Dict[str, int]] = collections.defaultdict(
        lambda: {"eligible": 0, "model_loses": 0}
    )
    by_minutes: Dict[str, Dict[str, int]] = collections.defaultdict(
        lambda: {"eligible": 0, "model_loses": 0}
    )
    by_thesis: Dict[str, Dict[str, Any]] = collections.defaultdict(
        lambda: {"eligible": 0, "model_loses": 0, "thesis_wrong": 0, "baseline_sum": 0.0}
    )
    for r in eligible_rows:
        by_edge[r["edge_bucket"]]["eligible"] += 1
        by_minutes[r["minutes_bucket"]]["eligible"] += 1
        by_thesis[r["thesis"]]["eligible"] += 1
        by_thesis[r["thesis"]]["baseline_sum"] += float(r.get("thesis_baseline") or 0.0)
        if r["model_loses"]:
            by_edge[r["edge_bucket"]]["model_loses"] += 1
            by_minutes[r["minutes_bucket"]]["model_loses"] += 1
            by_thesis[r["thesis"]]["model_loses"] += 1
        if not r["thesis_correct"]:
            by_thesis[r["thesis"]]["thesis_wrong"] += 1

    # Thesis skill is the hit rate MINUS what riding the base rate already pays.
    n_rows = len(eligible_rows)
    thesis_correct_n = sum(1 for r in eligible_rows if r["thesis_correct"])
    baseline_sum = sum(float(r.get("thesis_baseline") or 0.0) for r in eligible_rows)
    correct_rate = (thesis_correct_n / n_rows) if n_rows else None
    baseline_rate = (baseline_sum / n_rows) if n_rows else None
    excess_rate = (
        correct_rate - baseline_rate
        if correct_rate is not None and baseline_rate is not None
        else None
    )

    return {
        "resolved_contracts_replayed": None,  # filled by caller
        "eligible_contracts": len(eligible_rows),
        "thesis_correct_n": thesis_correct_n,
        "thesis_correct_rate": correct_rate,
        "thesis_baseline_rate": baseline_rate,
        "thesis_excess_rate": excess_rate,
        "min_eligible_for_trust": ce.MIN_ELIGIBLE_CONTRACTS,
        "model_loses_n": len(losses),
        "model_beats_n": len(wins),
        "mean_brier_eligible_model": _mean([r["model_brier"] for r in eligible_rows]),
        "mean_brier_eligible_mid": _mean([r["mid_brier"] for r in eligible_rows]),
        "mean_brier_delta_losses": _mean([r["brier_delta"] for r in losses]),
        "mean_edge_losses": _mean([r["mean_edge"] for r in losses]),
        "mean_minutes_left_losses": _mean([r["mean_minutes_left"] for r in losses]),
        "by_edge_bucket": dict(sorted(by_edge.items())),
        "by_minutes_bucket": dict(sorted(by_minutes.items(), key=lambda kv: kv[0])),
        "by_thesis": dict(sorted(by_thesis.items())),
        "worst_losses": sorted(losses, key=lambda r: -r["brier_delta"])[:10],
        "note": (
            "Bid postmortem covers fills; this covers calibrator eligible "
            "population (contracts the bot would size if trusted)."
        ),
    }


def load_state(path: Optional[Path] = None) -> dict:
    state_path = path or (Path(evidence_dir()) / STATE_NAME)
    with state_path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def run_autopsy(state: dict) -> Dict[str, Any]:
    record = state.get("resolved_record") or []
    eligible_rows = walk_eligible(record)
    summary = summarize(eligible_rows)
    summary["resolved_contracts_replayed"] = len(record)
    summary["state_eligible_scored"] = len(state.get("contract_scores_eligible_full") or [])
    stored_model = _mean(state.get("contract_scores_eligible_full") or [])
    stored_mid = _mean(state.get("contract_scores_eligible_market_mid_raw") or [])
    summary["state_mean_brier_eligible_model"] = stored_model
    summary["state_mean_brier_eligible_mid"] = stored_mid
    return summary


def _fmt(x: Optional[float], digits: int = 4) -> str:
    if x is None:
        return "n/a"
    return f"{x:.{digits}f}"


def _pct(x: Optional[float], signed: bool = False) -> str:
    if x is None:
        return "n/a"
    return f"{x:+.1%}" if signed else f"{x:.1%}"


def print_human(summary: Dict[str, Any]) -> None:
    print("ELIGIBLE-LOSS AUTOPSY (threshold / KXBTCD — not bid postmortem)")
    print(
        f"  resolved replayed {summary['resolved_contracts_replayed']} · "
        f"eligible {summary['eligible_contracts']}/{summary['min_eligible_for_trust']} "
        f"(state scores {summary['state_eligible_scored']})"
    )
    print(
        f"  eligible Brier model {_fmt(summary['mean_brier_eligible_model'])} vs "
        f"mid {_fmt(summary['mean_brier_eligible_mid'])} · "
        f"model loses {summary['model_loses_n']} / beats {summary['model_beats_n']}"
    )
    print(
        f"  on losses: mean ΔBrier {_fmt(summary['mean_brier_delta_losses'])} · "
        f"mean |edge| {_fmt(summary['mean_edge_losses'], 3)} · "
        f"mean minutes left {_fmt(summary['mean_minutes_left_losses'], 1)}"
    )
    print("  by edge:")
    for bucket, counts in summary["by_edge_bucket"].items():
        print(f"    {bucket}: lose {counts['model_loses']}/{counts['eligible']}")
    print("  by runway:")
    for bucket, counts in summary["by_minutes_bucket"].items():
        print(f"    {bucket}: lose {counts['model_loses']}/{counts['eligible']}")
    print(
        f"  thesis correct {summary['thesis_correct_n']}/{summary['eligible_contracts']} "
        f"= {_pct(summary['thesis_correct_rate'])} vs "
        f"zero-skill baseline {_pct(summary['thesis_baseline_rate'])} · "
        f"excess {_pct(summary['thesis_excess_rate'], signed=True)}"
    )
    print("  by thesis side:")
    for side, counts in summary["by_thesis"].items():
        eligible = counts["eligible"]
        side_baseline = (counts["baseline_sum"] / eligible) if eligible else None
        side_correct = eligible - counts["thesis_wrong"]
        side_rate = (side_correct / eligible) if eligible else None
        print(
            f"    {side}: lose {counts['model_loses']}/{eligible} · "
            f"thesis wrong {counts['thesis_wrong']} · "
            f"correct {_pct(side_rate)} vs baseline {_pct(side_baseline)}"
        )
    print(f"  {summary['note']}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--state", type=Path, default=None, help="override state path")
    args = parser.parse_args(argv)
    try:
        state = load_state(args.state)
    except FileNotFoundError as exc:
        print(f"eligible-autopsy: missing state: {exc}", file=sys.stderr)
        return 3
    summary = run_autopsy(state)
    if args.json:
        # Drop worst_losses detail clutter for machine? Keep it — useful.
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_human(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
