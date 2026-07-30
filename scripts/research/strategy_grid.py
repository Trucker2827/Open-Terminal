#!/usr/bin/env python3
"""Kalshi paper strategy-grid: a reproducible sweep over a pre-registered grid of
mechanical + signal-gated paper strategies, marked to the retained real book and
settled on real outcomes net of fees. Read-only; emits versioned evidence artifacts.
See docs/design/2026-07-30-kalshi-strategy-grid-design.md.

    python3 scripts/research/strategy_grid.py
"""
import bisect, collections, json, math, os, sys
import kalshi_edge_common as common
import q1_quote_lag as q1

SCHEMA_VERSION = 1
SIDES = ("YES", "NO")
ENTRY_BANDS = ((0.02, 0.10), (0.10, 0.25), (0.25, 0.50),
               (0.50, 0.75), (0.75, 0.90), (0.90, 1.01))
GATES = ("mechanical", "physics", "calibrator")
TPS = (0.05, 0.10, 0.15, 0.20, 0.30)
SLS = (0.05, 0.10, 0.15, 0.20)
TRAILS = (0.05, 0.10, 0.15)
MIN_ENTRY_S_TO_CLOSE = 120

def _band_tag(lo, hi):
    return "b%d-%d" % (round(lo * 100), round(min(hi, 1.0) * 100))

def _exit_tag(ex):
    return ex["kind"] if ex["amount"] is None else "%s%d" % (ex["kind"], round(ex["amount"] * 100))

def variant_id(v):
    return "|".join((v["side"], _band_tag(v["band"][0], v["band"][1]),
                     v["gate"], _exit_tag(v["exit"])))

def _exits():
    out = [{"kind": "hold", "amount": None}]
    out += [{"kind": "tp", "amount": a} for a in TPS]
    out += [{"kind": "sl", "amount": a} for a in SLS]
    out += [{"kind": "trail", "amount": a} for a in TRAILS]
    return out

def build_grid():
    grid = []
    for side in SIDES:
        for lo, hi in ENTRY_BANDS:
            for gate in GATES:
                for ex in _exits():
                    v = {"side": side, "band": [lo, hi], "gate": gate, "exit": ex}
                    v["variant_id"] = variant_id(v)
                    grid.append(v)
    return grid

FEE = common.fee_per_contract

def bet_side_quotes(rows, side):
    """(ts, yes_bid, yes_ask) -> (ts, bet_bid, bet_ask). NO buys/sells the complement."""
    if side == "YES":
        return [(ts, b, a) for ts, b, a in rows]
    return [(ts, 1.0 - a, 1.0 - b) for ts, b, a in rows]   # NO: bid/ask flip & complement

def simulate_exit(path, i0, entry_ask, exit_rule, close_ms):
    """First (sell_bid, exit_ts) after i0 (before close) where the rule fires; taker at bid."""
    kind, amt = exit_rule["kind"], exit_rule["amount"]
    if kind == "hold":
        return None, None
    running_max = None
    for ts, bid, _ask in path[i0 + 1:]:
        if close_ms is not None and ts >= close_ms:
            break
        if kind == "tp" and bid >= entry_ask + amt:
            return bid, ts
        if kind == "sl" and bid <= entry_ask - amt:
            return bid, ts
        if kind == "trail":
            running_max = bid if running_max is None else max(running_max, bid)
            if bid <= running_max - amt:
                return bid, ts
    return None, None

def contract_pnl(path, i0, entry_ask, exit_rule, won, close_ms=None):
    hold_pnl = ((1.0 if won else 0.0) - entry_ask) - FEE(entry_ask)
    sell_bid, _ts = simulate_exit(path, i0, entry_ask, exit_rule, close_ms)
    if sell_bid is None:
        return {"pnl": hold_pnl, "exited": False, "hold_pnl": hold_pnl}
    pnl = (sell_bid - entry_ask) - FEE(entry_ask) - FEE(sell_bid)
    return {"pnl": pnl, "exited": True, "hold_pnl": hold_pnl}

def find_entry(path, lo, hi, close_ms):
    for i, (ts, _bid, ask) in enumerate(path):
        if close_ms is not None and (close_ms - ts) / 1000.0 < MIN_ENTRY_S_TO_CLOSE:
            continue
        if lo <= ask < hi:
            return i, ask
    return None

def gate_ok(gate, ticker, ts_ms, side, signals):
    if gate == "mechanical":
        return True
    if gate == "physics":
        return signals.physics_ok(ticker, ts_ms, side)
    return signals.calibrator_ok(ticker, ts_ms, side)   # None when un-gateable

def clustered_mean(values, clusters):
    n = len(values)
    if n == 0:
        return {"n": 0, "effective_n": 0.0, "mean": None, "se": None, "t": None, "ci95": [None, None]}
    mean = sum(values) / n
    sizes = collections.Counter(clusters)
    eff = (n * n) / sum(s * s for s in sizes.values())
    groups = collections.defaultdict(float)
    for v, c in zip(values, clusters):
        groups[c] += (v - mean)
    cr_var = sum(g * g for g in groups.values()) / (n * n)
    se = math.sqrt(cr_var)
    t = mean / se if se > 0 else None
    half = 1.96 * se
    return {"n": n, "effective_n": eff, "mean": mean, "se": se, "t": t,
            "ci95": [mean - half, mean + half]}

def _normal_sf(z):
    return 0.5 * math.erfc(z / math.sqrt(2.0))

def _two_sided_p(t):
    return 2.0 * _normal_sf(abs(t)) if t is not None else 1.0

def benjamini_hochberg(pvals, alpha=0.05):
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    rejected = [False] * m
    max_k = -1
    for rank, idx in enumerate(order, start=1):
        if pvals[idx] <= alpha * rank / m:
            max_k = rank
    for rank, idx in enumerate(order, start=1):
        if rank <= max_k:
            rejected[idx] = True
    return rejected

def _fold_bounds(sorted_close, folds=5):
    m = len(sorted_close)
    return [sorted_close[min(m - 1, (m * k) // folds)] for k in range(1, folds + 1)]

def walkforward_delta(records, key):
    if not records:
        return None
    closes = sorted(r["close_ms"] for r in records)
    last_train_close = _fold_bounds(closes)[-2] if len(closes) >= 5 else closes[0]
    test = [r for r in records if r["close_ms"] > last_train_close]
    if not test:
        return None
    return sum(r["pnl"] - r[key] for r in test) / len(test)

def score_variant(records):
    if not records:
        return {"n": 0}
    dh = [r["pnl"] - r["hold_pnl"] for r in records]
    dm = [r["pnl"] - r["market_pnl"] for r in records]
    clusters = [r["cluster"] for r in records]
    ch = clustered_mean(dh, clusters)
    return {
        "n": len(records),
        "delta_vs_hold": sum(dh) / len(dh),
        "delta_vs_market": sum(dm) / len(dm),
        "clustered": ch,
        "p_value": _two_sided_p(ch["t"]),
        "walkforward_delta": walkforward_delta(records, "hold_pnl"),
        "win_rate": sum(1 for r in records if r["won"]) / len(records),
    }

# ── Task 5: glue — signals, run, latest_summary, main ──────────────────────
CALIBRATOR_FRESHNESS_MS = 60_000
CALIBRATOR_EDGE = 0.03
SURVIVOR_MIN_EFF_N = 30
BAND_FAMILY_PREFIX = ("KXBTCD", "KXBTC15M")

class Signals:
    """Backward-looking gate signals. physics: recomputed from BRTI at entry;
    calibrator: nearest logged calibrated_p at/<= entry within freshness."""
    def __init__(self, brti, calib_by_ticker):
        self.brti = brti
        self.calib = calib_by_ticker            # ticker -> sorted [(ts_ms, calibrated_p, mid)]

    def physics_ok(self, ticker, ts_ms, side):
        parsed = common.parse_ticker(ticker)
        if parsed is None or parsed["strike"] is None:
            return None
        sample = self.brti.nearest(ts_ms)
        if sample is None:
            return None
        _t, spot, _avg = sample
        window = self.brti.window(ts_ms - 1_800_000, ts_ms)
        sigma = common.realized_vol_per_min_bps(window)
        minutes = (parsed["close_ms"] - ts_ms) / 60000.0
        p = common.implied_probability(spot, parsed["strike"], sigma or 0.0, minutes)
        if p is None:
            return None
        edge = (p - 0.5) if side == "YES" else (0.5 - p)   # physics favours the side
        return edge > 0.0

    def calibrator_ok(self, ticker, ts_ms, side):
        rows = self.calib.get(ticker)
        if not rows:
            return None
        ts = [r[0] for r in rows]
        idx = bisect.bisect_right(ts, ts_ms) - 1
        if idx < 0 or ts_ms - rows[idx][0] > CALIBRATOR_FRESHNESS_MS:
            return None
        _t, cp, mid = rows[idx]
        signed = (cp - mid) if side == "YES" else (mid - cp)
        return signed >= CALIBRATOR_EDGE


def _sign(x):
    return (x > 0) - (x < 0)


def _outcome_for(ticker, parsed, settlements, brti):
    """(outcome, source) — recorded settlement first, derived fallback for
    KXBTCD only (KXBTC15M carries no strike and cannot be derived)."""
    row = settlements.get(ticker)
    recorded = {"yes": True, "no": False}.get((row or {}).get("result")) if row else None
    if recorded is not None:
        return recorded, "recorded"
    if parsed["family"] == "KXBTCD":
        derived = common.derive_outcome(parsed, brti)
        if derived is not None:
            return derived, "derived"
    return None, None


def _load_calibrator():
    """ticker -> sorted [(ts_ms, calibrated_p, market_mid)] from the bot's own
    decision log — read-only, never `spot_calibrator.run_once()`."""
    rows, inventory = common.read_jsonl("kalshi-bot-decisions.jsonl")
    by_ticker = collections.defaultdict(list)
    for row in rows:
        ticker = row.get("ticker")
        ts_ms = row.get("ts_ms")
        cp = row.get("calibrated_p")
        mid = row.get("market_mid")
        if ticker is None or ts_ms is None or cp is None or mid is None:
            continue
        try:
            by_ticker[ticker].append((int(ts_ms), float(cp), float(mid)))
        except (TypeError, ValueError):
            continue
    for ticker in by_ticker:
        by_ticker[ticker].sort()
    return dict(by_ticker), inventory


def run(evidence):
    """The full pre-registered grid, swept over every retained contract with a
    resolved outcome, scored against two nulls (hold, market) and corrected
    for multiple comparisons. Read-only; `evidence` is accepted for interface
    symmetry with the other q*/f* scripts but every loader below resolves its
    own path through `kalshi_edge_common`, which already honours
    `OPENTERMINAL_EVIDENCE_DIR`."""
    quotes, quote_inventory, quote_dropped = q1.load_quote_series()
    brti, brti_inventory = common.load_brti()

    family_counts = collections.Counter()
    kept_tickers = []
    excluded_non_threshold_family = 0
    for ticker in quotes:
        parsed = common.parse_ticker(ticker)
        family = parsed["family"] if parsed is not None else ticker.split("-")[0]
        family_counts[family] += 1
        if family not in BAND_FAMILY_PREFIX:
            # Includes KXBTC -B band markets (issue #169 Q1): YES is not
            # monotone in spot for a band, so this grid does not price them.
            excluded_non_threshold_family += 1
            continue
        kept_tickers.append(ticker)

    settlements, settlement_inventory = common.load_settlements(interesting=set(kept_tickers))

    outcomes = {}
    outcome_source = collections.Counter()
    unresolved = 0
    for ticker in kept_tickers:
        parsed = common.parse_ticker(ticker)
        if parsed is None:
            unresolved += 1
            continue
        outcome, source = _outcome_for(ticker, parsed, settlements, brti)
        if outcome is None:
            unresolved += 1
            continue
        outcomes[ticker] = outcome
        outcome_source[source] += 1

    calib_by_ticker, decision_inventory = _load_calibrator()
    signals = Signals(brti, calib_by_ticker)

    grid = build_grid()
    groups = collections.OrderedDict()
    for v in grid:
        key = (v["side"], tuple(v["band"]), v["gate"])
        groups.setdefault(key, []).append(v)

    variant_records = collections.defaultdict(list)
    ungateable_counts = collections.Counter()
    contracts_considered = 0

    for ticker, won in outcomes.items():
        parsed = common.parse_ticker(ticker)
        close_ms = parsed["close_ms"]
        rows = quotes.get(ticker) or []
        two_sided = [(ts, bid, ask) for ts, bid, ask, _mid in rows]
        if not two_sided:
            continue
        contracts_considered += 1
        path_by_side = {side: bet_side_quotes(two_sided, side) for side in SIDES}
        entry_cache = {}
        for (side, band, gate), variants in groups.items():
            path = path_by_side[side]
            entry_key = (side, band)
            if entry_key not in entry_cache:
                entry_cache[entry_key] = find_entry(path, band[0], band[1], close_ms)
            entry = entry_cache[entry_key]
            if entry is None:
                continue
            i0, entry_ask = entry
            ts_entry = path[i0][0]
            gate_res = gate_ok(gate, ticker, ts_entry, side, signals)
            if gate_res is None:            # ungateable: no fresh signal — counted, not gated out
                for v in variants:
                    ungateable_counts[v["variant_id"]] += 1
                continue
            if not gate_res:                # gated out: signal disagrees with the side
                continue
            side_won = won if side == "YES" else (not won)
            entry_bid = path[i0][1]
            entry_mid = (entry_bid + entry_ask) / 2.0
            # market_pnl: a market-priced baseline — holding at the bet-side
            # mid rather than the taker ask, still net of the fee at that mid.
            market_pnl = (1.0 if side_won else 0.0) - entry_mid - FEE(entry_mid)
            cluster = close_ms // 3_600_000    # settlement hour
            for v in variants:
                pnl_res = contract_pnl(path, i0, entry_ask, v["exit"], side_won, close_ms)
                variant_records[v["variant_id"]].append({
                    "pnl": pnl_res["pnl"], "hold_pnl": pnl_res["hold_pnl"],
                    "market_pnl": market_pnl, "won": side_won,
                    "cluster": cluster, "close_ms": close_ms,
                })

    scored = []
    pvals = []
    for v in grid:
        records = variant_records.get(v["variant_id"], [])
        score = score_variant(records)
        pval = score.get("p_value", 1.0)   # no data -> no evidence of an edge
        scored.append((v, score))
        pvals.append(pval)

    rejected = benjamini_hochberg(pvals, alpha=0.05) if pvals else []

    variants_out = []
    survivor_count = 0
    for idx, (v, score) in enumerate(scored):
        clustered = score.get("clustered", {})
        effective_n = clustered.get("effective_n", 0.0) or 0.0
        delta_vs_hold = score.get("delta_vs_hold")
        delta_vs_market = score.get("delta_vs_market")
        wf = score.get("walkforward_delta")
        survives = bool(rejected[idx])
        is_survivor = (survives and effective_n >= SURVIVOR_MIN_EFF_N and
                      wf is not None and delta_vs_hold is not None and
                      _sign(wf) == _sign(delta_vs_hold))
        if is_survivor:
            trust = "measured"
            survivor_count += 1
        elif effective_n < SURVIVOR_MIN_EFF_N:
            trust = "insufficient_sample"
        else:
            trust = "rejected"
        record = dict(v)
        record.update({
            "n_contracts": score.get("n", 0),
            "effective_n": effective_n,
            "delta_vs_hold": delta_vs_hold,
            "delta_vs_market": delta_vs_market,
            "walkforward_delta": wf,
            "win_rate": score.get("win_rate"),
            "ci95": clustered.get("ci95", [None, None]),
            "p_value": score.get("p_value", 1.0),
            "ungateable": ungateable_counts.get(v["variant_id"], 0),
            "survives_correction": survives,
            "trust": trust,
        })
        variants_out.append(record)

    return {
        "schema_version": SCHEMA_VERSION,
        "as_of_utc": common.as_of(),
        "data": {
            "quote_inventory": quote_inventory,
            "quote_rows_dropped": quote_dropped,
            "brti_inventory": brti_inventory,
            "settlement_inventory": settlement_inventory,
            "decision_inventory": decision_inventory,
            "quote_markets_by_family": dict(family_counts),
            "markets_excluded_non_threshold_family": excluded_non_threshold_family,
            "markets_with_resolved_outcome": len(outcomes),
            "outcome_source_counts": dict(outcome_source),
            "markets_unresolved": unresolved,
            "contracts_considered": contracts_considered,
            "calibrator_tickers_with_signal": len(calib_by_ticker),
            "survivor_count": survivor_count,
        },
        "method": {
            "families": list(BAND_FAMILY_PREFIX),
            "grid_size": len(grid),
            "cluster_key": "close_ms // 3_600_000 (contract's settlement hour)",
            "market_pnl": ("(1 if won else 0) - entry_mid - fee(entry_mid); "
                           "entry_mid is the bet-side mid at entry"),
            "correction": ("Benjamini-Hochberg over every variant's p-value on "
                           "delta_vs_hold, alpha=0.05"),
            "survivor_rule": (
                "survives_correction AND effective_n >= %d AND "
                "sign(walkforward_delta) == sign(delta_vs_hold)" % SURVIVOR_MIN_EFF_N),
            "trust_levels": ["measured", "insufficient_sample", "rejected"],
        },
        "variants": variants_out,
    }


def latest_summary(full):
    """The compact, honest survivors file: only variants trusted as
    'measured' are named, with the humility fields a reader needs to judge
    them, never the full sweep."""
    survivors = [v for v in full["variants"] if v.get("trust") == "measured"]
    survivors.sort(key=lambda v: v.get("delta_vs_hold") or 0.0, reverse=True)
    if survivors:
        headline = ("%d variant%s beat%s hold+market after correction"
                    % (len(survivors), "" if len(survivors) == 1 else "s",
                       "s" if len(survivors) == 1 else ""))
    else:
        headline = "no variant beats hold+market after correction"
    out_survivors = [{
        "variant_id": v["variant_id"],
        "side": v["side"],
        "band": v["band"],
        "gate": v["gate"],
        "exit": v["exit"],
        "delta_vs_hold": v["delta_vs_hold"],
        "delta_vs_market": v["delta_vs_market"],
        "effective_n": v["effective_n"],
        "ci95": v.get("ci95"),
        "walkforward_delta": v.get("walkforward_delta"),
        "trust": v["trust"],
    } for v in survivors]
    return {
        "schema_version": full["schema_version"],
        "as_of_utc": full["as_of_utc"],
        "headline": headline,
        "survivors": out_survivors,
    }


def _atomic_write_json(path, payload):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True, default=str)
    os.replace(tmp, path)


def _render_report(full, latest):
    lines = [
        "# Kalshi strategy grid — %s" % full["as_of_utc"],
        "",
        "grid size: %d variants" % full["method"]["grid_size"],
        "markets with resolved outcome: %d (source counts: %s)"
        % (full["data"]["markets_with_resolved_outcome"],
           full["data"]["outcome_source_counts"]),
        "markets excluded (non-threshold family): %d"
        % full["data"]["markets_excluded_non_threshold_family"],
        "markets unresolved: %d" % full["data"]["markets_unresolved"],
        "",
        "## %s" % latest["headline"],
    ]
    for s in latest["survivors"]:
        lines.append("  - %s  delta_vs_hold=%.4f  delta_vs_market=%.4f  effective_n=%.1f"
                     % (s["variant_id"], s["delta_vs_hold"] or 0.0,
                        s["delta_vs_market"] or 0.0, s["effective_n"]))
    return "\n".join(lines)


def main():
    full = run(common.evidence_dir())
    latest = latest_summary(full)
    _atomic_write_json(common.evidence_path("kalshi-strategy-grid.json"), full)
    _atomic_write_json(common.evidence_path("kalshi-strategy-grid-latest.json"), latest)
    print(_render_report(full, latest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
