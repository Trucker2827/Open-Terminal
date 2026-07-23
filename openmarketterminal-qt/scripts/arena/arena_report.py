#!/usr/bin/env python3
"""Alpha Arena leaderboard report.

Mechanical, preregistered, honest:
  - Brier (blind commitments vs settlements) comes from the terminal's own
    `advise score` verb, filtered per lane by provider+model.
  - Coverage comes from the arena round journal: committed / offered. A lane
    below the coverage floor is NEVER ranked — it is listed NOT_COMPARABLE
    with the reason (the v4 duel lesson: timeouts select easy cases).
  - No lane, no round, no number is fabricated. Missing data reads as
    missing.

Usage: arena_report.py [--min-coverage 0.8] [--min-resolved 50] [--out PATH]
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "kalshi_advise")))

from arena_loop import EVIDENCE_DIR, ROUNDS_PATH, load_registry
from advise_challenge import DEFAULT_CLI

DEFAULT_OUT = os.path.join(EVIDENCE_DIR, "arena-report.json")

HOW_IT_WORKS = (
    "Every model sees the same facts at the same instant — never the market's "
    "odds. Each seals its probability before anyone can peek. Kalshi settles "
    "the truth within the hour. Lower Brier = better forecaster. A model that "
    "skips too many rounds (coverage below the floor) is not ranked — skipping "
    "the hard ones must not look like skill.")


def read_rounds(path=ROUNDS_PATH):
    rounds = []
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    try:
                        rounds.append(json.loads(line))
                    except ValueError:
                        continue
    except OSError:
        pass
    return rounds


def coverage_from_rounds(rounds):
    """{lane_id: {"offered": n, "committed": n, "abstained": n, "expired": n}}"""
    stats = {}
    for rnd in rounds:
        if rnd.get("status") != "DONE":
            continue
        for lane in rnd.get("lanes", []):
            s = stats.setdefault(lane.get("id"), {"offered": 0, "committed": 0,
                                                  "abstained": 0, "expired": 0})
            s["offered"] += 1
            status = lane.get("status")
            if status == "COMMITTED_BLIND":
                s["committed"] += 1
            elif status == "ABSTAINED":
                s["abstained"] += 1
            elif status == "EXPIRED":
                s["expired"] += 1
    return stats


def cli_score(lane, cli=DEFAULT_CLI, profile=None):
    """Per-lane Brier via the terminal's advise score verb. None on failure."""
    cmd = [cli, "--json", "--headless"]
    if profile:
        cmd += ["--profile", profile]
    cmd += ["kalshi", "auto", "advise", "score",
            "--provider", f"arena-{lane['kind']}", "--model", lane["model"]]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        return json.loads(r.stdout) if r.stdout.strip() else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def build_report(lanes, rounds, score_fn, min_coverage, min_resolved, now_ms):
    coverage = coverage_from_rounds(rounds)
    entries = []
    for lane in lanes:
        cov = coverage.get(lane["id"], {"offered": 0, "committed": 0,
                                        "abstained": 0, "expired": 0})
        offered = cov["offered"]
        cov_ratio = (cov["committed"] / offered) if offered else None
        score = score_fn(lane) or {}
        resolved = ((score.get("coverage") or {}).get("resolved")
                    if isinstance(score.get("coverage"), dict) else 0) or 0
        entry = {
            "id": lane["id"], "model": lane["model"], "epoch_id": lane["epoch_id"],
            "offered": offered, "committed": cov["committed"],
            "abstained": cov["abstained"], "expired": cov["expired"],
            "coverage": cov_ratio, "resolved": resolved,
            "brier": score.get("brier_pre"),
            "brier_ci": [score.get("ci_low"), score.get("ci_high")],
        }
        if offered == 0:
            entry.update(comparable=False, reason="no rounds offered yet")
        elif cov_ratio is not None and cov_ratio < min_coverage:
            entry.update(comparable=False,
                         reason=f"coverage {cov_ratio:.0%} below {min_coverage:.0%} floor")
        elif resolved < min_resolved:
            entry.update(comparable=False,
                         reason=f"only {resolved}/{min_resolved} commitments resolved")
        else:
            entry.update(comparable=True, reason=None)
        entries.append(entry)

    ranked = sorted([e for e in entries if e["comparable"]],
                    key=lambda e: (e["brier"] if e["brier"] is not None else 9.9))
    for pos, e in enumerate(ranked, start=1):
        e["rank"] = pos
    verdict = "INSUFFICIENT_DATA"
    if len(ranked) >= 2:
        top, second = ranked[0], ranked[1]
        top_hi = top["brier_ci"][1]
        second_lo = second["brier_ci"][0]
        if (top["brier"] is not None and second["brier"] is not None
                and top_hi is not None and second_lo is not None
                and top_hi < second_lo):
            verdict = f"LEADER: {top['id']}"
        else:
            verdict = "STATISTICAL_TIE_SO_FAR"
    return {
        "schema": 1, "event": "arena_report", "advisory_only": True,
        "generated_at_ms": now_ms,
        "how_it_works": HOW_IT_WORKS,
        "thresholds": {"min_coverage": min_coverage, "min_resolved": min_resolved},
        "rounds_total": sum(1 for r in rounds if r.get("status") == "DONE"),
        "verdict": verdict,
        "leaderboard": entries,
    }


def main():
    ap = argparse.ArgumentParser(description="Alpha Arena leaderboard report")
    ap.add_argument("--min-coverage", type=float, default=0.80)
    ap.add_argument("--min-resolved", type=int, default=50)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cli", default=DEFAULT_CLI)
    ap.add_argument("--profile", default=None)
    args = ap.parse_args()

    lanes = load_registry()
    rounds = read_rounds()
    report = build_report(lanes, rounds,
                          lambda lane: cli_score(lane, args.cli, args.profile),
                          args.min_coverage, args.min_resolved,
                          int(time.time() * 1000))
    tmp = args.out + ".tmp"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2)
    os.replace(tmp, args.out)
    print(json.dumps({"out": args.out, "verdict": report["verdict"],
                      "rounds": report["rounds_total"],
                      "lanes": [e["id"] for e in report["leaderboard"]]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
