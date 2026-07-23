#!/usr/bin/env python3
"""Alpha Arena round loop — N models, one blind packet, sealed probabilities.

A round (group):
  1. Pick the freshest Kalshi contract with enough runway (same picker the
     duel's wrapper uses).
  2. `advise open` lane 1 with a fresh group id -> immutable blind context +
     challenge. `advise open --sibling-of` every other lane: the repository
     copies lane 1's exact context AND open instant, so no lane is penalized
     by call order and every lane shares one context_hash.
  3. Defense-in-depth: re-verify the context leaks no price field.
  4. All lanes forecast IN PARALLEL under one symmetric timeout.
  5. Each in-time predict is committed blind; slow lanes are left to EXPIRE
     honestly; abstains are recorded as abstains.
  6. The round is journaled (arena-rounds.jsonl) and the leaderboard report
     refreshed.

Advisory/shadow-only: nothing here can place orders. The frozen v5 duel files
are imported, never modified.

Usage:  arena_loop.py once | run [--interval 60]
"""
import argparse
import concurrent.futures
import json
import os
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "kalshi_advise")))
sys.path.insert(0, HERE)

import arena_llm_forecaster as adapter
from advise_challenge import (DEFAULT_CLI, DEFAULT_EVIDENCE, advise_commit_blind,
                              advise_open, contains_forbidden_deep, pick_auto_ticker)

REGISTRY_PATH = os.path.join(HERE, "forecasters.json")
EVIDENCE_DIR = os.path.dirname(os.path.expanduser(
    os.environ.get("OPENTERMINAL_ARENA_EVIDENCE", DEFAULT_EVIDENCE)))
ROUNDS_PATH = os.path.join(EVIDENCE_DIR, "arena-rounds.jsonl")


def load_registry(path=REGISTRY_PATH):
    with open(path, encoding="utf-8") as fh:
        reg = json.load(fh)
    defaults = reg.get("defaults", {})
    lanes = []
    for entry in reg.get("forecasters", []):
        if not entry.get("enabled", False):
            continue
        merged = dict(defaults)
        merged.update(entry)
        for key in ("id", "kind", "model", "endpoint", "epoch_id"):
            if not merged.get(key):
                raise ValueError(f"forecaster missing required key '{key}': {entry}")
        lanes.append(merged)
    if len(lanes) < 2:
        raise ValueError(f"arena needs >= 2 enabled forecasters, found {len(lanes)}")
    return lanes


def append_round(record, path=ROUNDS_PATH):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")


def run_round(cli=DEFAULT_CLI, profile=None, evidence=DEFAULT_EVIDENCE,
              min_secs_left=901, max_age_s=11.0):
    lanes = load_registry()
    now_ms = int(time.time() * 1000)
    round_rec = {"event": "arena_round", "group_id": str(uuid.uuid4()),
                 "opened_at_ms": now_ms, "advisory_only": True, "lanes": []}

    ticker = pick_auto_ticker(evidence, min_secs_left, max_age_s)
    if not ticker:
        round_rec.update(status="NO_CONTRACT",
                         reason="no fresh contract with enough runway")
        append_round(round_rec)
        return round_rec
    round_rec["ticker"] = ticker

    # Open lane 1, then siblings — all share lane 1's context + open instant.
    identities = [adapter.identify(lane) for lane in lanes]
    group_id = round_rec["group_id"]
    opened = []
    first = advise_open(cli, profile, ticker, identities[0],
                        competition_pair_id=group_id)
    if first.get("available") is False:
        round_rec.update(status="OPEN_REFUSED", reason=first.get("reason"))
        append_round(round_rec)
        return round_rec
    opened.append(first)
    for identity in identities[1:]:
        opened.append(advise_open(cli, profile, ticker, identity,
                                  competition_pair_id=group_id,
                                  sibling_of=first.get("challenge_id")))

    ctx = first.get("context") or {}
    leak = contains_forbidden_deep(ctx)
    if leak:
        round_rec.update(status="FIREWALL_BREACH", reason=f"forbidden key '{leak}'")
        append_round(round_rec)
        raise SystemExit(f"FIREWALL BREACH: blind context contained '{leak}' — aborting")

    ttl_ms = int(first.get("prediction_ttl_ms") or 0)
    safety_ms = max(int(lane.get("safety_margin_ms", 6000)) for lane in lanes)
    t_open = time.monotonic()

    def lane_forecast(idx):
        lane, opened_row = lanes[idx], opened[idx]
        forecast = adapter.predict(lane, ctx)
        elapsed_ms = int((time.monotonic() - t_open) * 1000)
        lane_rec = {"id": lane["id"], "epoch_id": lane["epoch_id"],
                    "model": lane["model"],
                    "model_digest": identities[idx].get("model_digest", ""),
                    "challenge_id": opened_row.get("challenge_id"),
                    "prompt_hash": forecast.get("prompt_hash"),
                    "elapsed_ms": elapsed_ms,
                    "decision": forecast.get("decision"),
                    "reason_code": forecast.get("reason_code")}
        if forecast.get("decision") != "predict":
            lane_rec["status"] = "ABSTAINED"
            return lane_rec
        remaining_ms = ttl_ms - int((time.monotonic() - t_open) * 1000)
        if remaining_ms < safety_ms:
            lane_rec.update(status="EXPIRED",
                            reason=f"too slow ({elapsed_ms}ms of {ttl_ms}ms TTL)")
            return lane_rec
        res = advise_commit_blind(cli, profile, opened_row.get("challenge_id"),
                                  str(uuid.uuid4()), float(forecast["probability"]),
                                  float(forecast.get("confidence", -1)),
                                  forecast.get("rationale", ""))
        lane_rec.update(status="COMMITTED_BLIND",
                        commit_state=res.get("state"))
        return lane_rec

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(lanes)) as pool:
        round_rec["lanes"] = list(pool.map(lane_forecast, range(len(lanes))))

    committed = sum(1 for l in round_rec["lanes"] if l.get("status") == "COMMITTED_BLIND")
    round_rec.update(status="DONE", committed=committed, ttl_ms=ttl_ms,
                     context_keys=sorted(ctx.keys()))
    append_round(round_rec)
    return round_rec


def main():
    ap = argparse.ArgumentParser(description="Alpha Arena round loop")
    ap.add_argument("command", choices=["once", "run"])
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--cli", default=DEFAULT_CLI)
    ap.add_argument("--profile", default=None)
    ap.add_argument("--evidence", default=DEFAULT_EVIDENCE)
    args = ap.parse_args()

    if args.command == "once":
        rec = run_round(args.cli, args.profile, args.evidence)
        print(json.dumps(rec))
        return 0 if rec.get("status") in ("DONE", "NO_CONTRACT") else 1

    import subprocess
    report_script = os.path.join(HERE, "arena_report.py")
    while True:
        try:
            rec = run_round(args.cli, args.profile, args.evidence)
            print(json.dumps({"round": rec.get("status"),
                              "ticker": rec.get("ticker"),
                              "committed": rec.get("committed")}), flush=True)
            subprocess.run([sys.executable, report_script, "--cli", args.cli],
                           capture_output=True, timeout=120)
        except SystemExit:
            raise
        except Exception as exc:  # keep the loop alive on transient failures
            print(json.dumps({"round": "ERROR", "error": str(exc)[:300]}), flush=True)
        time.sleep(max(15, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
