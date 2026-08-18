#!/usr/bin/env python3
"""Hash-locked BTC 1h Chronos/control shadow comparison.

Reads paper forecast rows and BTC ticks from the local research database. It
never imports an order client or calls a venue. One matched observation feeds
four immutable views: Chronos, control, agreement, and conflict. Agreement and
conflict measure the control strategy's result in those regimes; the record
also preserves the corresponding Chronos result.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import sqlite3
import tempfile
import time

POLICY = {
    "symbol": "BTC-USD",
    "horizon_ms": 3_600_000,
    "pair_tolerance_ms": 15 * 60_000,
    "chronos_source": "chronos2-forecast",
    "chronos_horizon": "1h",
    "control_source": "edge crypto-recommend",
    "control_horizon": "3600s",
    "notional_usd": 2.0,
    "round_trip_cost_rate": 0.017,
    "cohorts": ["chronos_alone", "control_alone", "agreement", "conflict"],
    "agreement_semantics": "control return when directions agree",
    "conflict_semantics": "control return when directions conflict",
    "validation_minimum": 50,
    "validation_target": 100,
    "authority": "paper_research_only_no_order_api",
}


def policy_hash():
    raw = json.dumps(POLICY, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def default_db_path():
    return os.path.expanduser(
        "~/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db")


def default_state_path():
    return os.path.expanduser(
        "~/Library/Application Support/Open Terminal/Open Terminal/btc1h-chronos-shadow.json")


def initial_state(now_ms):
    return {
        "schema": 1,
        "event": "btc1h_chronos_shadow_state",
        "policy": POLICY,
        "policy_sha256": policy_hash(),
        "frozen_at_ms": int(now_ms),
        "records": {},
        "diagnostics": {"runs": 0, "skip_reasons": {}, "last_run": {}},
    }


def atomic_write(path, payload):
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".btc1h-chronos-shadow-", dir=directory, text=True)
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


def normalize_direction(row):
    direction = str(row.get("direction") or "").strip().lower()
    if direction in ("up", "down"):
        return direction
    probability = row.get("model_probability")
    if probability is not None:
        return "up" if float(probability) >= 0.5 else "down"
    return None


def read_signals(connection, source, horizon, since_ms):
    rows = connection.execute(
        """SELECT id,created_at,direction,side,gate,model_probability,confidence,
                  features_json
             FROM edge_decision_journal
            WHERE source=? AND symbol=? AND horizon=? AND created_at>=?
            ORDER BY created_at""",
        (source, POLICY["symbol"], horizon, int(since_ms))).fetchall()
    names = [item[0] for item in connection.execute(
        "SELECT id,created_at,direction,side,gate,model_probability,confidence,features_json "
        "FROM edge_decision_journal LIMIT 0").description]
    return [dict(zip(names, row)) for row in rows]


def pair_signals(chronos_rows, control_rows):
    """Return one causal pair per UTC hour, without reusing a control row."""
    controls = list(control_rows)
    used = set()
    pairs = []
    seen_hours = set()
    for chronos in chronos_rows:
        direction = normalize_direction(chronos)
        if direction is None:
            continue
        hour = int(chronos["created_at"]) // POLICY["horizon_ms"]
        if hour in seen_hours:
            continue
        candidates = [row for row in controls
                      if row["id"] not in used
                      and normalize_direction(row) is not None
                      and abs(int(row["created_at"]) - int(chronos["created_at"]))
                          <= POLICY["pair_tolerance_ms"]]
        if not candidates:
            continue
        control = min(candidates,
                      key=lambda row: abs(int(row["created_at"]) - int(chronos["created_at"])))
        used.add(control["id"])
        seen_hours.add(hour)
        pairs.append((hour, chronos, control))
    return pairs


def nearest_tick(connection, target_ms, *, after=True):
    operator = ">=" if after else "<="
    order = "ASC" if after else "DESC"
    row = connection.execute(
        f"""SELECT price,exchange_ts,received_ts,source
              FROM edge_prediction_raw_ticks
             WHERE symbol IN (?, ?) AND price>0
               AND (CASE WHEN exchange_ts>0 THEN exchange_ts ELSE received_ts END) {operator} ?
             ORDER BY (CASE WHEN exchange_ts>0 THEN exchange_ts ELSE received_ts END) {order}
             LIMIT 1""", (POLICY["symbol"], POLICY["symbol"].split("-", 1)[0],
                            int(target_ms))).fetchone()
    if not row:
        return None
    return {"price": float(row[0]), "ts_ms": int(row[1] or row[2]), "source": row[3]}


def _diagnostics(state):
    return state.setdefault("diagnostics", {"runs": 0, "skip_reasons": {}, "last_run": {}})


def _skip(diag, reason, count=1):
    diag["skip_reasons"][reason] = diag["skip_reasons"].get(reason, 0) + count
    diag["last_run"][reason] = diag["last_run"].get(reason, 0) + count


def add_pairs(state, connection):
    diag = _diagnostics(state)
    diag["runs"] += 1
    diag["last_run"] = {}
    chronos = read_signals(connection, POLICY["chronos_source"],
                           POLICY["chronos_horizon"], state["frozen_at_ms"])
    controls = read_signals(connection, POLICY["control_source"],
                            POLICY["control_horizon"], state["frozen_at_ms"])
    diag["last_run"]["chronos_rows"] = len(chronos)
    diag["last_run"]["control_rows"] = len(controls)
    pairs = pair_signals(chronos, controls)
    paired_chronos = {row[1]["id"] for row in pairs}
    valid_chronos = [row for row in chronos if normalize_direction(row) is not None]
    _skip(diag, "invalid_chronos_direction", len(chronos) - len(valid_chronos))
    _skip(diag, "no_control_within_tolerance",
          sum(row["id"] not in paired_chronos for row in valid_chronos))
    for hour, chrono, control in pairs:
        key = str(hour)
        if key in state["records"]:
            _skip(diag, "duplicate_hour")
            continue
        pair_ts = max(int(chrono["created_at"]), int(control["created_at"]))
        entry = nearest_tick(connection, pair_ts, after=True)
        if entry is None:
            _skip(diag, "missing_reference_tick")
            continue
        chrono_direction = normalize_direction(chrono)
        control_direction = normalize_direction(control)
        state["records"][key] = {
            "status": "open",
            "hour_bucket": hour,
            "pair_ts_ms": pair_ts,
            "resolve_after_ms": pair_ts + POLICY["horizon_ms"],
            "entry": entry,
            "chronos": {"journal_id": chrono["id"], "created_at": chrono["created_at"],
                        "direction": chrono_direction, "confidence": chrono["confidence"]},
            "control": {"journal_id": control["id"], "created_at": control["created_at"],
                        "direction": control_direction, "confidence": control["confidence"],
                        "gate": control["gate"], "side": control["side"]},
            "relationship": "agreement" if chrono_direction == control_direction else "conflict",
        }
        diag["last_run"]["opened"] = diag["last_run"].get("opened", 0) + 1


def directional_pnl(direction, entry_price, exit_price):
    raw_return = (exit_price / entry_price) - 1.0
    signed = raw_return if direction == "up" else -raw_return
    return POLICY["notional_usd"] * (signed - POLICY["round_trip_cost_rate"])


def settle(state, connection, now_ms):
    for row in state["records"].values():
        if row["status"] != "open" or now_ms < row["resolve_after_ms"]:
            continue
        exit_tick = nearest_tick(connection, row["resolve_after_ms"], after=True)
        if exit_tick is None or exit_tick["ts_ms"] > now_ms:
            _skip(_diagnostics(state), "missing_settlement_tick")
            continue
        entry_price = row["entry"]["price"]
        row["exit"] = exit_tick
        row["chronos_pnl"] = directional_pnl(
            row["chronos"]["direction"], entry_price, exit_tick["price"])
        row["control_pnl"] = directional_pnl(
            row["control"]["direction"], entry_price, exit_tick["price"])
        actual = "up" if exit_tick["price"] >= entry_price else "down"
        row["actual_direction"] = actual
        row["chronos_won"] = row["chronos"]["direction"] == actual
        row["control_won"] = row["control"]["direction"] == actual
        row["status"] = "completed"
        row["settled_at_ms"] = now_ms
        row["postmortem"] = {
            "entry_price": entry_price, "exit_price": exit_tick["price"],
            "return_bps": ((exit_tick["price"] / entry_price) - 1.0) * 10_000,
            "relationship": row["relationship"],
            "chronos_direction": row["chronos"]["direction"],
            "control_direction": row["control"]["direction"],
            "actual_direction": actual,
            "chronos_confidence": row["chronos"].get("confidence"),
            "control_confidence": row["control"].get("confidence"),
        }
        diag = _diagnostics(state)
        diag["last_run"]["settled"] = diag["last_run"].get("settled", 0) + 1


def cohort_rows(state, cohort):
    rows = [row for row in state["records"].values() if row["status"] == "completed"]
    if cohort == "agreement":
        rows = [row for row in rows if row["relationship"] == "agreement"]
    elif cohort == "conflict":
        rows = [row for row in rows if row["relationship"] == "conflict"]
    return sorted(rows, key=lambda row: row["settled_at_ms"])


def cohort_summary(state, cohort):
    rows = cohort_rows(state, cohort)
    pnl_key = "chronos_pnl" if cohort == "chronos_alone" else "control_pnl"
    win_key = "chronos_won" if cohort == "chronos_alone" else "control_won"
    equity = peak = drawdown = 0.0
    for row in rows:
        equity += row[pnl_key]
        peak = max(peak, equity)
        drawdown = max(drawdown, peak - equity)
    return {"completed": len(rows), "net_pnl": equity,
            "win_rate": (sum(bool(row[win_key]) for row in rows) / len(rows) if rows else None),
            "max_drawdown": drawdown}


def summarize(state):
    cohorts = {name: cohort_summary(state, name) for name in POLICY["cohorts"]}
    paired = cohorts["control_alone"]["completed"]
    return {"authority": POLICY["authority"], "policy_sha256": state["policy_sha256"],
            "frozen_at_ms": state["frozen_at_ms"], "paired_completed": paired,
            "open": sum(row["status"] == "open" for row in state["records"].values()),
            "validation_minimum": POLICY["validation_minimum"],
            "validation_target": POLICY["validation_target"], "cohorts": cohorts,
            "diagnostics": _diagnostics(state)}


def run_once(state_path, db_path=None, now_ms=None):
    db_path = db_path or default_db_path()
    now_ms = int(time.time() * 1000) if now_ms is None else int(now_ms)
    lock_path = os.path.abspath(state_path) + ".lock"
    os.makedirs(os.path.dirname(os.path.abspath(state_path)), exist_ok=True)
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        state, created = load_state(state_path, now_ms)
        if not created:
            connection = sqlite3.connect(f"file:{os.path.abspath(db_path)}?mode=ro", uri=True)
            try:
                add_pairs(state, connection)
                settle(state, connection, now_ms)
            finally:
                connection.close()
        state["updated_at"] = dt.datetime.fromtimestamp(
            now_ms / 1000, tz=dt.timezone.utc).isoformat()
        atomic_write(state_path, state)
        return {"created": created, "state_path": os.path.abspath(state_path),
                "policy": POLICY, "summary": summarize(state)}
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--state", default=default_state_path())
    parser.add_argument("--db", default=default_db_path())
    args = parser.parse_args(argv)
    result = run_once(args.state, args.db)
    print(json.dumps(result, sort_keys=True, indent=None if args.json else 2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
