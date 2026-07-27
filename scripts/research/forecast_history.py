#!/usr/bin/env python3
"""Reconstruct a per-forecast resolved history for the calibrator (issue #169).

WHY THIS EXISTS. The calibrator's own record cannot answer "where is the error
concentrated?", because `spot-calibrator-state.json` keeps only the trailing
`BRIER_WINDOW = 500` `[probability, outcome]` pairs and attaches no features,
no ticker and no timestamp to any of them (see `spot_calibrator.settle_cycle`).
Worse for interpretation, `settle_cycle` appends ONE PAIR PER OBSERVATION and
each contract contributes up to `MAX_OBS_PER_TICKER = 60` of them, so those 500
"training_samples" are roughly 8–33 contracts' worth of heavily correlated rows.

The bot's decision log carries what the state file discards: for every tick it
logged the ticker, the wall clock, the calibrator's `calibrated_p` and the
`market_mid` it was compared against. Joining that to an outcome per contract
rebuilds the per-forecast history with the metadata attached — which is what
lets the report slice error by time-to-expiry, time of day, volatility regime
and distance from the strike.

Outcomes come from two sources and every row says which:
  `recorded` — the public settlement feed, Kalshi's own result. Preferred.
  `derived`  — the BRTI 60-second average at close versus the ticker's strike,
               used only where the settlement feed has no row. Its fidelity is
               measured against every recorded settlement it can reach and the
               caller is expected to print that agreement rate beside any table
               that derived rows contribute to.
Contracts resolvable by neither are DROPPED and counted, never imputed.

Read-only: opens evidence, writes nothing.
"""
import collections
import math

import kalshi_edge_common as common

BOT_DECISIONS = "kalshi-bot-decisions.jsonl"
VOL_GRID_MS = 60_000          # volatility recomputed once a minute, then reused
VOL_LOOKBACK_MS = 30 * 60_000  # trailing window for realized volatility


class VolatilityGrid:
    """Trailing realized volatility on a one-minute grid, computed once.

    Recomputing a 30-minute window per decision row would be ~88k passes over
    the BRTI series for no extra resolution: the underlying estimate barely
    moves inside a minute. Grid points with too few BRTI samples stay None and
    every consumer treats that as missing.
    """

    def __init__(self, brti):
        self.brti = brti
        self.cache = {}

    def at(self, ts_ms):
        key = ts_ms // VOL_GRID_MS
        if key not in self.cache:
            window = self.brti.window(key * VOL_GRID_MS - VOL_LOOKBACK_MS,
                                      key * VOL_GRID_MS)
            self.cache[key] = common.realized_vol_per_min_bps(window)
        return self.cache[key]


def load_decisions():
    """Every bot decision row that carries a usable calibrator forecast."""
    records, inventory = common.read_jsonl(BOT_DECISIONS, rotations=("{name}",))
    rows = []
    dropped = collections.Counter()
    for record in records:
        ticker = record.get("ticker")
        p = record.get("calibrated_p")
        mid = record.get("market_mid")
        ts_ms = record.get("ts_ms")
        if not ticker or p is None or mid is None or ts_ms is None:
            dropped["missing_fields"] += 1
            continue
        try:
            p = float(p)
            mid = float(mid)
            ts_ms = int(ts_ms)
        except (TypeError, ValueError):
            dropped["unparseable"] += 1
            continue
        if not 0.0 <= p <= 1.0 or not 0.0 <= mid <= 1.0:
            dropped["out_of_range"] += 1
            continue
        rows.append({"ticker": ticker, "ts_ms": ts_ms,
                     "calibrated_p": p, "market_mid": mid,
                     "action": record.get("action"),
                     "reason_code": record.get("reason_code")})
    return rows, inventory, dict(dropped)


def build(brti=None, brti_inventory=None):
    """The reconstructed history plus the audit needed to publish it.

    Returns a dict with `rows` (one per forecast observation, feature-tagged),
    `contracts` (one per settled contract — the effective sample), and the
    counts every table in the report has to be able to cite.
    """
    if brti is None:
        brti, brti_inventory = common.load_brti()
    decisions, decision_inventory, dropped = load_decisions()

    tickers = {row["ticker"] for row in decisions}
    settlements, settlement_audit = common.load_settlements(interesting=tickers)
    validation = common.validate_settlement_rule(settlements, brti)

    parsed_by_ticker = {}
    unparseable = 0
    for ticker in tickers:
        parsed = common.parse_ticker(ticker)
        if parsed is None:
            unparseable += 1
            continue
        parsed_by_ticker[ticker] = parsed

    outcomes = {}
    source_counts = collections.Counter()
    for ticker, parsed in parsed_by_ticker.items():
        row = settlements.get(ticker)
        recorded = {"yes": True, "no": False}.get((row or {}).get("result"))
        if recorded is not None:
            outcomes[ticker] = (recorded, "recorded")
            source_counts["recorded"] += 1
            continue
        derived = common.derive_outcome(parsed, brti)
        if derived is not None:
            outcomes[ticker] = (derived, "derived")
            source_counts["derived"] += 1
        else:
            source_counts["unresolvable"] += 1

    volatility = VolatilityGrid(brti)
    rows = []
    per_contract = collections.defaultdict(list)
    skipped_after_close = 0
    for decision in decisions:
        ticker = decision["ticker"]
        parsed = parsed_by_ticker.get(ticker)
        resolved = outcomes.get(ticker)
        if parsed is None or resolved is None:
            continue
        outcome, source = resolved
        seconds_left = (parsed["close_ms"] - decision["ts_ms"]) / 1000.0
        if seconds_left <= 0:
            # A forecast logged at or after the close is not a forecast; the
            # outcome is already determined and scoring it would flatter the
            # calibrator with hindsight.
            skipped_after_close += 1
            continue
        minutes_left = seconds_left / 60.0
        sigma = volatility.at(decision["ts_ms"])
        spot_sample = brti.nearest(decision["ts_ms"], max_gap_ms=30_000)
        spot = spot_sample[1] if spot_sample else None
        distance_sigma = None
        if (spot is not None and parsed["strike"] is not None
                and sigma and sigma > 0.0 and minutes_left > 0.0):
            move = spot * (sigma / 10000.0) * math.sqrt(minutes_left)
            if move > 0.0:
                distance_sigma = (spot - parsed["strike"]) / move
        row = dict(decision)
        row.update({"outcome": outcome, "outcome_source": source,
                    "family": parsed["family"], "strike": parsed["strike"],
                    "close_ms": parsed["close_ms"],
                    "seconds_left": seconds_left,
                    "minutes_left": minutes_left,
                    "hour_utc": common.iso(decision["ts_ms"])[11:13],
                    "vol_per_min_bps": sigma,
                    "spot": spot,
                    "distance_sigma": distance_sigma})
        rows.append(row)
        per_contract[ticker].append(row)

    contracts = []
    for ticker, contract_rows in sorted(per_contract.items()):
        contract_rows.sort(key=lambda r: r["ts_ms"])
        contracts.append({"ticker": ticker,
                          "family": contract_rows[0]["family"],
                          "outcome": contract_rows[0]["outcome"],
                          "outcome_source": contract_rows[0]["outcome_source"],
                          "close_ms": contract_rows[0]["close_ms"],
                          "observations": len(contract_rows),
                          "rows": contract_rows})

    obs_counts = sorted(c["observations"] for c in contracts)
    return {
        "rows": rows,
        "contracts": contracts,
        "audit": {
            "decision_files": decision_inventory,
            "brti_files": brti_inventory,
            "brti_samples": len(brti),
            "brti_span_utc": [common.iso(t) for t in brti.span_ms],
            "decision_rows_with_forecast": len(decisions),
            "decision_rows_dropped": dropped,
            "distinct_tickers_watched": len(tickers),
            "tickers_unparseable": unparseable,
            "outcome_sources": dict(source_counts),
            "settlement_feed": settlement_audit,
            "derived_rule_validation": validation,
            "forecast_rows_scored": len(rows),
            "forecast_rows_dropped_at_or_after_close": skipped_after_close,
            "contracts_scored": len(contracts),
            "observations_per_contract": {
                "min": obs_counts[0] if obs_counts else None,
                "median": obs_counts[len(obs_counts) // 2] if obs_counts else None,
                "max": obs_counts[-1] if obs_counts else None,
            },
        },
    }
