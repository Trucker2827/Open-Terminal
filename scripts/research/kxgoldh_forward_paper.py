#!/usr/bin/env python3
"""Hash-locked, paper-only KXGOLDH hourly trial.

One family, one 10-cent executable-edge floor, hold to settlement. Never
reads 15m or silver/oil reports. Never calls an order API. Creation of the
state file is a clean freeze: older calibrator snapshots cannot enter.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import tempfile
import time

import kalshi_edge_common as common
from kxbtc15m_underdog_cashout import batch_fee, size_for_cap

FAMILY = "KXGOLDH"
REPORT_STEM = "commodities-hourly-calibrator"
POLICY = {
    "family": FAMILY,
    "horizon": "hourly",
    "minimum_executable_edge": 0.10,
    "max_entry_all_in_usd": 2.0,
    "entry_price_min": 0.05,
    "entry_price_max": 0.95,
    "max_report_age_ms": 20 * 60_000,
    "selection": "one highest-edge KXGOLDH strike per event",
    "exit": "hold to settlement",
    "authority": "paper_research_only_no_order_api",
}


def policy_hash():
    encoded = json.dumps(POLICY, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def initial_state(now_ms):
    return {
        "schema": 1,
        "event": "kxgoldh_forward_paper_state",
        "policy": POLICY,
        "policy_sha256": policy_hash(),
        "frozen_at_ms": int(now_ms),
        "records": {},
    }


def atomic_write(path, payload):
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".kxgoldh-forward-", dir=directory, text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def load_state(path, now_ms):
    if not os.path.exists(path):
        return initial_state(now_ms), True
    with open(path, encoding="utf-8") as handle:
        state = json.load(handle)
    if state.get("policy_sha256") != policy_hash() or state.get("policy") != POLICY:
        raise RuntimeError("paper policy mismatch; refusing to tune an existing trial")
    return state, False


def event_of(ticker):
    return ticker.rsplit("-T", 1)[0] if "-T" in ticker else ticker


def candidate(ticker, pred):
    if not str(ticker).startswith(FAMILY + "-"):
        return None
    try:
        probability = float(pred["p_yes_full"])
    except (KeyError, TypeError, ValueError):
        return None
    choices = []
    if pred.get("market_yes_ask") is not None:
        ask = float(pred["market_yes_ask"])
        choices.append((probability - ask, "YES", ask))
    if pred.get("market_yes_bid") is not None:
        bid = float(pred["market_yes_bid"])
        choices.append((bid - probability, "NO", 1.0 - bid))
    choices = [row for row in choices
               if POLICY["entry_price_min"] <= row[2] <= POLICY["entry_price_max"]]
    if not choices:
        return None
    edge, side, price = max(choices)
    if edge < POLICY["minimum_executable_edge"]:
        return None
    return {
        "ticker": ticker,
        "event_ticker": event_of(ticker),
        "side": side,
        "entry_price": price,
        "model_probability": probability if side == "YES" else 1.0 - probability,
        "executable_edge": edge,
    }


def best_by_event(predictions):
    best = {}
    for ticker, pred in predictions.items():
        found = candidate(ticker, pred)
        if found is None:
            continue
        previous = best.get(found["event_ticker"])
        if previous is None or found["executable_edge"] > previous["executable_edge"]:
            best[found["event_ticker"]] = found
    return list(best.values())


def gold_predictions(report):
    family = (report.get("by_family") or {}).get(FAMILY) or {}
    return family.get("predictions") or {}


def outcomes_from_hourly_state():
    path = common.evidence_path(REPORT_STEM + "-state.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as handle:
        state = json.load(handle)
    family = (state.get("by_family") or {}).get(FAMILY) or {}
    out = {}
    for row in family.get("resolved_record") or []:
        ticker = row.get("ticker")
        if ticker and row.get("outcome") is not None:
            out[ticker] = bool(row["outcome"])
    return out


def settle(state, outcomes, now_ms):
    for row in state["records"].values():
        if row["status"] != "open" or row["ticker"] not in outcomes:
            continue
        won = outcomes[row["ticker"]] == (row["side"] == "YES")
        payout = row["contracts"] if won else 0.0
        row.update(
            status="completed",
            outcome_yes=outcomes[row["ticker"]],
            won=won,
            settled_at_ms=now_ms,
            net_pnl=payout - row["entry_all_in"],
        )


def summarize(state):
    rows = list(state["records"].values())
    completed = sorted(
        (row for row in rows if row["status"] == "completed"),
        key=lambda row: row.get("settled_at_ms") or 0)
    equity = peak = drawdown = 0.0
    for row in completed:
        equity += row["net_pnl"]
        peak = max(peak, equity)
        drawdown = max(drawdown, peak - equity)
    return {
        "frozen_at_ms": state["frozen_at_ms"],
        "policy_sha256": state["policy_sha256"],
        "family": FAMILY,
        "completed": len(completed),
        "open": sum(row["status"] == "open" for row in rows),
        "net_pnl": sum(row["net_pnl"] for row in completed),
        "win_rate": (sum(row["won"] for row in completed) / len(completed)
                     if completed else None),
        "max_drawdown": drawdown,
        "validation_target": 100,
        "remaining": max(0, 100 - len(completed)),
        "authority": POLICY["authority"],
    }


def _apply_report(state, report, now_ms):
    generated = int(report.get("generated_at_ms") or 0)
    if generated < state["frozen_at_ms"]:
        return
    if now_ms - generated > POLICY["max_report_age_ms"]:
        return
    for found in best_by_event(gold_predictions(report)):
        key = found["event_ticker"]
        if key in state["records"]:
            continue
        quantity = size_for_cap(found["entry_price"], POLICY["max_entry_all_in_usd"])
        if quantity < 1:
            continue
        fee = batch_fee(found["entry_price"], quantity)
        state["records"][key] = {
            **found,
            "horizon": "hourly",
            "family": FAMILY,
            "minimum_edge": POLICY["minimum_executable_edge"],
            "status": "open",
            "observed_at_ms": now_ms,
            "entry_ts_ms": now_ms,
            "report_generated_at_ms": generated,
            "contracts": quantity,
            "entry_fee": fee,
            "entry_all_in": quantity * found["entry_price"] + fee,
        }


def run_once(path, now_ms=None, report=None, outcomes=None):
    now_ms = int(time.time() * 1000) if now_ms is None else int(now_ms)
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    lock_path = os.path.abspath(path) + ".lock"
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        state, created = load_state(path, now_ms)
        if not created:
            if report is None:
                report_path = common.evidence_path(REPORT_STEM + ".json")
                with open(report_path, encoding="utf-8") as handle:
                    report = json.load(handle)
            _apply_report(state, report, now_ms)
            settle(state, outcomes if outcomes is not None else outcomes_from_hourly_state(),
                   now_ms)
        state["updated_at"] = dt.datetime.fromtimestamp(
            now_ms / 1000, tz=dt.timezone.utc).isoformat()
        atomic_write(path, state)
        return {"created": created, "state_path": os.path.abspath(path),
                "policy": POLICY, "summary": summarize(state)}
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)


def held_bid(side, yes_bid, yes_ask):
    return yes_bid if side == "YES" else 1.0 - yes_ask


def postmortem_trades(records, quotes):
    """Classify completed fills from the held-side bid path. Read-only."""
    out = []
    for record in records:
        if record.get("status") != "completed":
            continue
        ticker = record["ticker"]
        side = record["side"]
        price = record["entry_price"]
        entry_ts = record.get("entry_ts_ms") or record.get("observed_at_ms")
        path = [row for row in quotes.get(ticker) or [] if row[0] >= (entry_ts or 0)]
        bids = [held_bid(side, row[1], row[2]) for row in path]
        hit10 = any(bid + 1e-12 >= price * 1.10 for bid in bids)
        won = bool(record.get("won") or (record.get("net_pnl") or 0) > 0)
        if won and hit10:
            kind = "win_had_10pct_bid"
        elif won:
            kind = "win_settlement_only"
        elif hit10:
            kind = "loss_gave_10pct_then_died"
        elif bids and max(bids) > price + 0.005:
            kind = "loss_briefly_green"
        else:
            kind = "loss_never_green"
        out.append({
            "ticker": ticker,
            "side": side,
            "entry_price": price,
            "net_pnl": record.get("net_pnl"),
            "won": won,
            "executable_edge": record.get("executable_edge"),
            "hit_10pct_bid": hit10,
            "max_favorable": (max(bid - price for bid in bids) if bids else None),
            "max_adverse": (min(bid - price for bid in bids) if bids else None),
            "classification": kind,
            "path_quotes": len(path),
        })
    return out


def load_gold_quotes():
    records, _ = common.read_jsonl("kalshi-tickers.jsonl")
    by_ticker = {}
    for row in records:
        ticker = str(row.get("market_ticker") or "")
        if not ticker.startswith(FAMILY + "-"):
            continue
        try:
            ts_ms = int(float(row.get("ts_ms")))
        except (TypeError, ValueError):
            continue
        try:
            yes_bid = float(row["yes_bid_dollars"])
            yes_ask = float(row["yes_ask_dollars"])
        except (KeyError, TypeError, ValueError):
            continue
        if not (0 <= yes_bid <= yes_ask <= 1):
            continue
        by_ticker.setdefault(ticker, {})[ts_ms] = (ts_ms, yes_bid, yes_ask)
    return {ticker: [by_ticker[ticker][key] for key in sorted(by_ticker[ticker])]
            for ticker in by_ticker}


def run_postmortem(state_path):
    with open(state_path, encoding="utf-8") as handle:
        state = json.load(handle)
    if state.get("policy_sha256") != policy_hash():
        raise RuntimeError("frozen policy hash mismatch")
    completed = [row for row in state.get("records", {}).values()
                 if row.get("status") == "completed"]
    trades = postmortem_trades(completed, load_gold_quotes())
    winners = [row for row in trades if row["won"]]
    losers = [row for row in trades if not row["won"]]
    return {
        "event": "kxgoldh_forward_postmortem",
        "read_only": True,
        "policy_sha256": state.get("policy_sha256"),
        "completed": len(completed),
        "analyzable": len(trades),
        "winner_n": len(winners),
        "loser_n": len(losers),
        "classifications": {key: sum(row["classification"] == key for row in trades)
                            for key in sorted({row["classification"] for row in trades})},
        "trades": trades,
    }


def default_state_path():
    return common.evidence_path("kxgoldh-forward-paper.json")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", default=default_state_path())
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--postmortem", action="store_true",
                        help="read-only path replay of completed paper fills")
    args = parser.parse_args(argv)
    result = run_postmortem(args.state) if args.postmortem else run_once(args.state)
    print(json.dumps(result, indent=None if args.json else 2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
