#!/usr/bin/env python3
"""Kalshi WEATHER decision producer (daily city high-temp markets).

Validated edge (2026-08-02): daily city high-temp brackets on Kalshi are
mispriced on close calls (market Brier ~0.225, near coin-flip), while an
Open-Meteo morning-of forecast (bias-corrected, per-city ~2F error) predicts
better (Brier ~0.211) and trading it nets ~+10c/contract OOS after real
ask+fee, surviving a train/test holdout and a lookahead check.

This producer mirrors the crypto `kalshi auto-plan` path: it computes
forecast-implied probabilities for near-money weather brackets and, where the
forecast disagrees with the live market by > MARGIN, emits a decision. In
--write mode it (a) writes an evidence JSON to the evidence dir and (b) inserts
gate='pass' rows into edge_decision_journal with source='kalshi weather-plan',
which the existing sandbox PaperExecutor opens as PAPER positions (managed
exits / LOCK_WIN apply). PAPER ONLY — there is no live rung for weather.

NOTE (architecture): the established pattern is Python emits evidence JSON and
the C++ CLI writes the journal. Writing the journal directly here is an INTERIM
choice to stand up the paper loop fast; TODO migrate to a C++ writer command
(mirroring kalshi_auto_journal_plan) once the edge is confirmed on paper.

Usage:
  weather_producer.py            # DRY RUN — print decisions, write nothing
  weather_producer.py --write    # write evidence JSON + gate='pass' journal rows
"""
import sys, os, json, math, time, uuid, argparse, urllib.request
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from openterminal_paths import journal_db, evidence_file  # noqa: E402

BASE = "https://external-api.kalshi.com/trade-api/v2"
SOURCE = "kalshi weather-plan"
MARGIN = 0.08          # forecast must beat the price by this to trade
RESERVE = 0.01         # exit-cost reserve subtracted into gate_edge
# The backtest measured the edge on GENUINELY-UNCERTAIN brackets (market mid in
# 0.15-0.85) at the ~15h morning-of lead. Match that regime exactly: a bracket
# priced near 0/1 (or a market that has already sharpened late-day) produces
# huge apparent "edges" that are really forecast-vs-sharp-market noise near a
# bracket boundary, NOT the validated signal.
MID_LO, MID_HI = 0.15, 0.85          # uncertain-bracket band (on the mid)
LEAD_MIN, LEAD_MAX = 36000, 61200    # 10-17h before close (centered on the ~15h backtest lead)

# series -> (label, lat, lon, bias_F, std_F)  bias/std MEASURED empirically (fc_quality)
CITIES = {
    "KXHIGHNY":   ("NYC-HIGH",   40.78, -73.97,  1.2, 2.1),
    "KXHIGHCHI":  ("CHI-HIGH",   41.96, -87.93, -0.4, 2.0),
    "KXHIGHDEN":  ("DEN-HIGH",   39.85, -104.66,-1.2, 1.2),
    "KXHIGHTSFO": ("SFO-HIGH",   37.62, -122.37, 0.5, 2.7),
    "KXHIGHPHIL": ("PHIL-HIGH",  39.87, -75.23, -2.2, 1.4),
    "KXHIGHTSEA": ("SEA-HIGH",   47.44, -122.31,-0.6, 2.0),
}


def http_json(url, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=20) as r:
                return json.load(r)
        except Exception:
            if i == tries - 1:
                return None
            time.sleep(0.4)
    return None


def ncdf(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))


def bracket_prob(fc, strike_type, floor, cap, std):
    """P(high in bracket) under Normal(fc, std). Integer-degree continuity ±0.5."""
    st = strike_type or ""
    if "greater" in st and floor is not None:
        return 1 - ncdf((floor + 0.5 - fc) / std)
    if "less" in st and cap is not None:
        return ncdf((cap + 0.5 - fc) / std)
    if st == "between" and floor is not None and cap is not None:
        return ncdf((cap + 0.5 - fc) / std) - ncdf((floor - 0.5 - fc) / std)
    return None


def kalshi_fee(price):
    if price <= 0 or price >= 1:
        return 0.0
    return 0.07 * price * (1 - price)


_FC = {}
def forecast_high(lat, lon, date):
    k = (lat, lon, date)
    if k in _FC:
        return _FC[k]
    d = http_json(f"https://historical-forecast-api.open-meteo.com/v1/forecast?"
                  f"latitude={lat}&longitude={lon}&start_date={date}&end_date={date}"
                  f"&daily=temperature_2m_max&temperature_unit=fahrenheit&timezone=America%2FNew_York")
    try:
        v = d["daily"]["temperature_2m_max"][0]
    except Exception:
        v = None
    _FC[k] = v
    return v


def book_bid_ask(ticker):
    """Real yes bid/ask from the live orderbook (list endpoint nulls the quote
    fields). Kalshi returns `orderbook_fp` with `yes_dollars`/`no_dollars`, each
    a list of [price_str, size_str] in DOLLARS. Best yes bid = max yes price;
    best yes ask = 1 - (max no price), since a no-bid is a yes-ask."""
    ob = http_json(f"{BASE}/markets/{ticker}/orderbook?depth=5")
    book = (ob or {}).get("orderbook_fp", {}) or {}
    yes = book.get("yes_dollars") or []
    no = book.get("no_dollars") or []
    yes_bid = max((float(p) for p, _ in yes), default=0.0)
    no_best = max((float(p) for p, _ in no), default=0.0)
    yes_ask = (1.0 - no_best) if no_best else 0.0
    ydepth = sum(float(s) for _, s in yes)
    ndepth = sum(float(s) for _, s in no)
    return yes_bid, yes_ask, ydepth, ndepth


def decisions_for_series(series, cfg, now_ms):
    label, lat, lon, bias, std = cfg
    out = []
    d = http_json(f"{BASE}/markets?series_ticker={series}&status=open&limit=200")
    if not d:
        return out
    markets = d.get("markets", [])
    # group by settlement day (from ticker YYMMMDD)
    from collections import defaultdict
    byday = defaultdict(list)
    for m in markets:
        parts = m.get("ticker", "").split("-")
        if len(parts) < 2:
            continue
        byday[parts[1]].append(m)
    import datetime
    for day_tag, ms in byday.items():
        try:
            day = datetime.datetime.strptime(day_tag, "%y%b%d").date().isoformat()
        except Exception:
            continue
        raw = forecast_high(lat, lon, day)
        if raw is None:
            continue
        fc = raw - bias
        for m in ms:
            st, floor, cap = m.get("strike_type"), m.get("floor_strike"), m.get("cap_strike")
            fp = bracket_prob(fc, st, floor, cap, std)
            if fp is None:
                continue
            fp = min(max(fp, 0.0), 1.0)
            ct = m.get("close_time", "")
            try:
                cts = int(datetime.datetime.fromisoformat(ct.replace("Z", "+00:00")).timestamp())
            except Exception:
                continue
            seconds_left = cts - now_ms // 1000
            # Trade only in the VALIDATED lead window (~15h before close, the
            # morning-of the settlement day, where the per-city forecast std was
            # calibrated). Day-ahead (>17h) is less certain than the std assumes;
            # late-day (<10h) the market has sharpened past the measured edge.
            if seconds_left < LEAD_MIN or seconds_left > LEAD_MAX:
                continue
            yes_bid, yes_ask, ydepth, ndepth = book_bid_ask(m["ticker"])
            if yes_ask <= 0 or yes_bid <= 0:  # need a two-sided live quote
                continue
            mid = (yes_bid + yes_ask) / 2.0
            if not (MID_LO <= mid <= MID_HI):  # only genuinely-uncertain brackets
                continue
            no_ask = 1.0 - yes_bid
            side = price = model_p = depth = None
            if fp > yes_ask + MARGIN:
                side, price, model_p, depth = "yes", yes_ask, fp, ydepth
            elif (1 - fp) > no_ask + MARGIN:
                side, price, model_p, depth = "no", no_ask, 1 - fp, ndepth
            if side is None:
                continue
            fee = kalshi_fee(price)
            raw_edge = model_p - price
            edge_after = raw_edge - fee
            out.append({
                "ticker": m["ticker"], "label": label, "symbol": label,
                "question": m.get("title", ""), "side": side, "day": day,
                "forecast_high": round(fc, 1), "forecast_prob_side": round(model_p, 4),
                "price": round(price, 4), "yes_bid": yes_bid, "no_bid": 1.0 - yes_ask,
                "raw_edge": round(raw_edge, 4), "edge_after_cost": round(edge_after, 4),
                "gate_edge": round(edge_after - RESERVE, 4), "fee": round(fee, 4),
                "spread": round(yes_ask - yes_bid, 4), "depth": depth,
                "seconds_left": seconds_left, "confidence": round(min(1.0, 2.0 / std), 3),
            })
        time.sleep(0.05)
    return out


def write_journal(rows, now_ms):
    import sqlite3
    con = sqlite3.connect(journal_db())
    cur = con.cursor()
    n = 0
    for r in rows:
        features = {"signal": {"yes_bid": r["yes_bid"], "no_bid": r["no_bid"],
                               "selected_depth": r["depth"], "model_confidence": r["confidence"]},
                    "forecast": {"high_f": r["forecast_high"], "prob_side": r["forecast_prob_side"]}}
        freshness = {"forecast_as_of_ms": now_ms, "source": "open-meteo"}
        cur.execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, venue, symbol, horizon,"
            " market_id, question, direction, side, call, gate, market_probability, model_probability,"
            " raw_edge, edge_after_cost, gate_edge, spread_cost, fee_cost, liquidity_score, confidence,"
            " seconds_left, data_status, freshness_json, features_json, reasons, outcome, resolved_at, source)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (str(uuid.uuid4()), now_ms, now_ms, "kalshi", r["symbol"], "daily",
             r["ticker"], r["question"], "weather", r["side"], "WEATHER FORECAST LEG", "pass",
             r["price"], r["forecast_prob_side"], r["raw_edge"], r["edge_after_cost"], r["gate_edge"],
             r["spread"], r["fee"], float(r["depth"] or 0), r["confidence"], r["seconds_left"],
             "trade_candidate", json.dumps(freshness), json.dumps(features),
             f"forecast {r['forecast_high']}F -> P({r['side']})={r['forecast_prob_side']:.2f} vs price {r['price']:.2f}",
             -1, 0, SOURCE))
        n += 1
    con.commit()
    con.close()
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="write journal rows + evidence (default: dry run)")
    args = ap.parse_args()
    now_ms = int(time.time() * 1000)
    all_rows = []
    for series, cfg in CITIES.items():
        rows = decisions_for_series(series, cfg, now_ms)
        all_rows.extend(rows)
        print(f"[{cfg[0]}] {series}: {len(rows)} decisions", file=sys.stderr)
    all_rows.sort(key=lambda r: -r["gate_edge"])
    print(f"\n=== WEATHER DECISIONS ({len(all_rows)}) — {'WRITE' if args.write else 'DRY RUN'} ===")
    for r in all_rows[:40]:
        print(f"  {r['ticker'][:26]:26} buy {r['side']:3} @ {r['price']:.2f}  "
              f"forecast_P={r['forecast_prob_side']:.2f} (high {r['forecast_high']}F)  "
              f"edge={r['edge_after_cost']:+.3f} gate_edge={r['gate_edge']:+.3f} "
              f"secs_left={r['seconds_left']}")
    evidence = {"event": "kalshi_weather_producer", "generated_at_ms": now_ms,
                "source": SOURCE, "margin": MARGIN, "decisions": all_rows}
    if args.write:
        with open(evidence_file("kalshi-weather-plan.json"), "w") as f:
            json.dump(evidence, f, indent=2)
        n = write_journal(all_rows, now_ms)
        print(f"\nWROTE {n} gate='pass' journal rows (source='{SOURCE}') + evidence JSON.")
    else:
        print(f"\n(dry run — nothing written; {len(all_rows)} decisions would be journaled with --write)")


if __name__ == "__main__":
    main()
