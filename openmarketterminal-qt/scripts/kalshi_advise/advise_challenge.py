#!/usr/bin/env python3
"""Scripted open -> commit-blind wrapper for the LLM advisory scoring trial.

The advisory firewall + short crypto-market TTLs impose a tight (~30s) window:
`advise open` only accepts a snapshot <=15s old, and prediction_ttl for crypto
strike markets is ~30s. A forecaster must therefore open and commit-blind almost
simultaneously. This wrapper does exactly that, atomically, with a hard timing
guard that NEVER commits a stale-window prediction.

Flow:
  1. Ask the forecaster for its identity (fast, no API call).
  2. `advise open` the contract, tagging the challenge with that identity.
  3. Defense-in-depth: verify the emitted blind context leaks no price field.
  4. Pipe the blind context to the forecaster; time the round trip.
  5. If enough of the prediction_ttl remains (> safety margin), `advise commit-blind`
     the returned probability. Otherwise ABORT — the challenge is left to expire
     honestly (recorded EXPIRED in the durable ledger), never committed late.

The forecaster is a pluggable command (see claude_forecaster.py for the contract).
The blind context handed to it is exactly what `advise open` emits — this wrapper
never fetches or forwards a price.

Examples:
  advise_challenge.py --ticker KXBTCD-26JUL2106-T66099.99
  advise_challenge.py --auto                    # pick the freshest openable contract
  advise_challenge.py --auto --dry-run          # open + forecast + timing, do NOT commit
  advise_challenge.py --ticker T --forecaster ./my_forecaster.py
"""
import argparse
import json
import os
import subprocess
import sys
import time
import uuid
from advisor_core import validate_forecast

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..")))
from openterminal_paths import evidence_file

DEFAULT_CLI = os.path.abspath(os.path.join(HERE, "..", "..", "build", "openterminalcli"))
DEFAULT_FORECASTER = os.path.join(HERE, "claude_forecaster.py")
DEFAULT_EVIDENCE = evidence_file("kalshi-ws-books.json")

# Mirrors adv::kBlindForbiddenKeys() — belt-and-suspenders re-check on the client
# side. `advise open` already excludes these; this catches any regression.
FORBIDDEN_KEYS = {
    "yes_bid", "yes_ask", "no_bid", "no_ask", "yes_depth", "no_depth",
    "market_implied_probability", "market_curve_probability", "fair_yes", "fair_no",
    "divergence", "daemon_probability", "calibrated_probability", "model_probability",
    "model_weight", "cost_net_edge", "execution",
}


def _run(cmd, stdin_text=None, timeout=None):
    return subprocess.run(
        cmd, input=stdin_text, capture_output=True, text=True, timeout=timeout
    )


def forecaster_identify(forecaster):
    r = _run([sys.executable, forecaster, "identify"], timeout=20)
    if r.returncode != 0:
        raise SystemExit(f"forecaster identify failed: {r.stderr.strip() or r.stdout.strip()}")
    return json.loads(r.stdout)


def contains_forbidden_deep(node):
    """Recursively return the first forbidden key found anywhere in node, or None."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k in FORBIDDEN_KEYS:
                return k
            hit = contains_forbidden_deep(v)
            if hit:
                return hit
    elif isinstance(node, list):
        for v in node:
            hit = contains_forbidden_deep(v)
            if hit:
                return hit
    return None


def pick_auto_ticker(evidence_path, min_secs_left, max_age_s):
    """Freshest contract that is both fresh (<max_age_s) and far enough from
    settlement (>min_secs_left). Returns a ticker or None."""
    import datetime
    try:
        d = json.load(open(evidence_path))
    except (OSError, json.JSONDecodeError):
        return None
    now_ms = int(time.time() * 1000)
    now = datetime.datetime.now(datetime.timezone.utc)
    cands = []
    for tk, snap in (d.get("snapshots") or {}).items():
        try:
            observed = int(str(snap.get("observed_at_ms", "0")) or 0)
        except ValueError:
            continue
        if observed <= 0:
            continue
        age = (now_ms - observed) / 1000.0
        if age >= max_age_s:
            continue
        ct = (snap.get("contract") or {}).get("close_time")
        secs_left = None
        if ct:
            try:
                dt = datetime.datetime.fromisoformat(ct.replace("Z", "+00:00"))
                secs_left = int((dt - now).total_seconds())
            except ValueError:
                secs_left = None
        if secs_left is not None and secs_left <= min_secs_left:
            continue
        cands.append((age, tk))
    cands.sort()
    return cands[0][1] if cands else None


def advise_open(cli, profile, ticker, identity, competition_pair_id=None, sibling_of=None):
    cmd = [cli, "--json", "--headless"]
    if profile:
        cmd += ["--profile", profile]
    cmd += ["kalshi", "auto", "advise", "open"]
    if ticker:
        cmd += ["--ticker", ticker]
    if competition_pair_id:
        cmd += ["--competition-pair-id", competition_pair_id]
    if sibling_of:
        cmd += ["--sibling-of", sibling_of]
    for flag, key in (("--provider", "provider"), ("--model", "model"),
                      ("--prompt-version", "prompt_version"), ("--agent-id", "agent_id"),
                      ("--run-id", "run_id")):
        val = identity.get(key)
        if val:
            cmd += [flag, str(val)]
    r = _run(cmd, timeout=40)
    if r.returncode != 0 and not r.stdout.strip():
        raise SystemExit(f"advise open failed: {r.stderr.strip()}")
    return json.loads(r.stdout)


def advise_commit_blind(cli, profile, challenge_id, commit_id, prob, conf, rationale):
    cmd = [cli, "--json", "--headless"]
    if profile:
        cmd += ["--profile", profile]
    cmd += ["kalshi", "auto", "advise", "commit-blind",
            "--challenge", challenge_id, "--commit-id", commit_id,
            "--probability", f"{prob:.6f}"]
    if conf is not None and conf >= 0:
        cmd += ["--confidence", f"{conf:.6f}"]
    if rationale:
        cmd += ["--rationale", rationale]
    r = _run(cmd, timeout=40)
    if r.returncode != 0 and not r.stdout.strip():
        raise SystemExit(f"advise commit-blind failed: {r.stderr.strip()}")
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description="Scripted advise open -> commit-blind wrapper")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--ticker", help="contract ticker to open")
    g.add_argument("--auto", action="store_true", help="auto-pick the freshest openable contract")
    ap.add_argument("--forecaster", default=DEFAULT_FORECASTER,
                    help="forecaster command (identify|predict contract)")
    ap.add_argument("--cli", default=DEFAULT_CLI, help="openterminalcli path")
    ap.add_argument("--profile", default=None, help="openterminalcli --profile")
    ap.add_argument("--evidence", default=DEFAULT_EVIDENCE, help="evidence file for --auto")
    ap.add_argument("--safety-margin-ms", type=int, default=6000,
                    help="abort if less than this much prediction_ttl remains at commit time")
    ap.add_argument("--auto-min-secs-left", type=int, default=75)
    ap.add_argument("--auto-max-age-s", type=float, default=11.0)
    ap.add_argument("--dry-run", action="store_true",
                    help="open + forecast + timing report, but do NOT commit-blind")
    args = ap.parse_args()

    identity = forecaster_identify(args.forecaster)

    ticker = args.ticker
    if args.auto:
        ticker = pick_auto_ticker(args.evidence, args.auto_min_secs_left, args.auto_max_age_s)
        if not ticker:
            print(json.dumps({"ok": False, "reason": "no fresh+runway contract available; pass --ticker"}))
            return 3

    opened = advise_open(args.cli, args.profile, ticker, identity)
    t_open = time.monotonic()
    if opened.get("available") is False:
        print(json.dumps({"ok": False, "reason": opened.get("reason"), "ticker": ticker}))
        return 3

    ctx = opened.get("context") or {}
    leak = contains_forbidden_deep(ctx)
    if leak:
        # Never hand a leaking context to the forecaster; abort loudly.
        raise SystemExit(f"FIREWALL BREACH: blind context contained forbidden key '{leak}' — aborting")

    challenge_id = opened.get("challenge_id")
    ttl_ms = int(opened.get("prediction_ttl_ms") or 0)

    pred_r = _run([sys.executable, args.forecaster, "predict"],
                  stdin_text=json.dumps(ctx), timeout=max(10, ttl_ms / 1000 + 20))
    if pred_r.returncode != 0:
        print(json.dumps({"ok": False, "challenge_id": challenge_id, "ticker": ticker,
                          "reason": "forecaster failed",
                          "detail": (pred_r.stderr.strip() or pred_r.stdout.strip())[:400]}))
        return 4
    pred = validate_forecast(json.loads(pred_r.stdout))
    if pred["decision"] == "abstain":
        print(json.dumps({"ok": True, "committed": False, "challenge_id": challenge_id,
                          "ticker": ticker, "forecaster": identity, "forecast": pred,
                          "reason": "forecaster abstained; challenge left to expire"}))
        return 0
    p_pre = float(pred["probability"])
    conf = float(pred.get("confidence", -1))
    rationale = pred.get("rationale", "")

    elapsed_ms = int((time.monotonic() - t_open) * 1000)
    remaining_ms = ttl_ms - elapsed_ms

    out = {
        "challenge_id": challenge_id, "ticker": ticker,
        "forecaster": identity, "p_pre": p_pre, "confidence": conf, "rationale": rationale,
        "prediction_ttl_ms": ttl_ms, "forecaster_elapsed_ms": elapsed_ms,
        "remaining_ms": remaining_ms, "safety_margin_ms": args.safety_margin_ms,
        "settlement_band": ctx.get("settlement_band"), "distance_bps": ctx.get("distance_bps"),
    }

    if remaining_ms < args.safety_margin_ms:
        out.update(ok=False, committed=False,
                   reason=f"forecaster too slow ({elapsed_ms}ms); window closed — challenge left to EXPIRE")
        print(json.dumps(out))
        return 5

    if args.dry_run:
        out.update(ok=True, committed=False, reason="dry-run: within window but not committing")
        print(json.dumps(out))
        return 0

    commit_id = str(uuid.uuid4())
    res = advise_commit_blind(args.cli, args.profile, challenge_id, commit_id,
                              p_pre, conf, rationale)
    out.update(ok=True, committed=True, commit_id=commit_id,
               commit_result={k: res.get(k) for k in ("state", "id", "ts_committed")})
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
