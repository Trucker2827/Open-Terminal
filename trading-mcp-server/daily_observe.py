#!/usr/bin/env python3
"""Headless daily BTC/ETH observation — OBSERVATION ONLY (never trades).

Run by launchd/cron. Pulls live data via the trading_mcp READ paths only, computes
a deterministic "trade lesson" (regime / relative strength / volatility / levels to
watch / a PAPER-ONLY idea / the honest reason for no action — all rule-based, no LLM),
appends it to trade-journal.md, persists a per-day snapshot to observe_history.jsonl
(ground truth for the weekly review), detects key-level breaks / RS flips / large moves
vs the prior run, and fires a best-effort macOS notification on any alert.

No LLM, no order placement, no settings changes — it imports the read tools and calls
only get_product / get_historical_data. An flock lockfile prevents two runs (e.g. a
launchd wake catch-up + a manual kickstart) from interleaving on the journal/state.
"""
import datetime
import fcntl
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)  # so .env and the trading_mcp package resolve regardless of the caller's cwd
sys.path.insert(0, str(HERE))

from trading_mcp.config import get_settings              # noqa: E402
from trading_mcp.safety import RiskManager               # noqa: E402
from trading_mcp.coinbase_tools import CoinbaseService   # noqa: E402
from trading_mcp.alpaca_tools import AlpacaService       # noqa: E402
from trading_mcp.thesis import (                         # noqa: E402
    classify_regime, vol_state, watch_levels, thesis_text, REGIME_LABEL,
)

STATE = HERE / "observe_state.json"
HISTORY = HERE / "observe_history.jsonl"
JOURNAL = HERE / "trade-journal.md"
LOCK = HERE / "observe.lock"            # shared with weekly_review.py
ALERT_24H_PCT = 8.0
HISTORY_DAYS = 90                        # enough for MA50 + a vol percentile


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def acquire_lock():
    """Non-blocking flock on observe.lock. Returns the open handle (keep it alive for
    the run; the OS releases it on process exit) or None if another run holds it."""
    f = open(LOCK, "w")
    try:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


def read_symbol(cb, al, cb_sym, al_sym):
    prod = cb.client.get_product(cb_sym)
    d = prod.to_dict() if hasattr(prod, "to_dict") else (prod if isinstance(prod, dict) else {})
    price = num(d.get("price"))
    chg24 = num(d.get("price_percentage_change_24h"))
    hd = al.get_historical_data(al_sym, "1Day", "crypto", HISTORY_DAYS)
    raw = hd.get("bars", {})
    bars = (raw.get("data", {}) or {}).get(al_sym) if isinstance(raw, dict) else None
    if not bars and isinstance(raw, dict):
        bars = raw.get(al_sym)
    closes = [c for c in (num(b.get("close")) for b in (bars or [])) if c is not None]
    # Use the live Coinbase print as the freshest "today" close so break detection
    # tests the current price, not a stale daily bar.
    if closes and price:
        closes = closes[:-1] + [price]
    hi30 = max(closes[-30:]) if closes else None
    lo30 = min(closes[-30:]) if closes else None
    pos = (price - lo30) / (hi30 - lo30) * 100 if (price and hi30 and lo30 and hi30 > lo30) else None
    r7 = (price / closes[-7] - 1) * 100 if (price and len(closes) >= 7) else None
    reg = classify_regime(closes) if closes else {"regime": "unclear"}
    return {"price": price, "chg24": chg24, "hi30": hi30, "lo30": lo30, "pos": pos, "r7": r7,
            "regime": reg.get("regime", "unclear"),
            "vol": vol_state(closes) if closes else None,
            "levels": watch_levels(closes) if closes else None}


def notify(title, msg):
    # Best-effort macOS notification. From launchd it may not reach the Aqua session;
    # the journal + log line are the reliable channels.
    try:
        subprocess.run(["osascript", "-e",
                        "display notification %s with title %s" % (json.dumps(msg), json.dumps(title))],
                       timeout=10, check=False)
    except Exception:
        pass


def detect_alerts(btc, eth, leader, prior):
    alerts = []
    for nm, cur, pr in [("BTC", btc, prior.get("btc", {})), ("ETH", eth, prior.get("eth", {}))]:
        if cur["price"] and pr.get("lo30") and cur["price"] < pr["lo30"]:
            alerts.append(f"{nm} broke support ${pr['lo30']:,.0f} (now ${cur['price']:,.0f})")
        if cur["price"] and pr.get("hi30") and cur["price"] > pr["hi30"]:
            alerts.append(f"{nm} broke resistance ${pr['hi30']:,.0f} (now ${cur['price']:,.0f})")
        if cur["chg24"] is not None and abs(cur["chg24"]) >= ALERT_24H_PCT:
            alerts.append(f"{nm} large 24h move {cur['chg24']:+.1f}%")
    if prior.get("leader") and prior["leader"] != leader:
        alerts.append(f"relative-strength leadership flipped {prior['leader']}→{leader}")
    return alerts


def _vol_str(v):
    return f"{v['label']} ({v['daily_pct']:.1f}%/day, {v['pctile']:.0f}th pctile)" if v else "n/a"


def _levels_str(lv):
    return (f"support ${lv['support']:,.0f} / MA20 ${lv['ma20']:,.0f} / resistance ${lv['resistance']:,.0f}"
            if lv else "n/a")


def build_lesson(btc, eth, leader):
    """The deterministic 'trade lesson' block — list of markdown lines."""
    bi, br = thesis_text(btc["regime"], btc["levels"]) if btc["levels"] else ("(no data)", "(no data)")
    ei, er = thesis_text(eth["regime"], eth["levels"]) if eth["levels"] else ("(no data)", "(no data)")
    return [
        f"- **Snapshot** — BTC ${btc['price']:,.0f} (24h {btc['chg24']:+.1f}%, 7d {btc['r7']:+.1f}%) · "
        f"ETH ${eth['price']:,.0f} (24h {eth['chg24']:+.1f}%, 7d {eth['r7']:+.1f}%)",
        f"- **Regime** — BTC: {REGIME_LABEL.get(btc['regime'], btc['regime'])} · "
        f"ETH: {REGIME_LABEL.get(eth['regime'], eth['regime'])}",
        f"- **Relative strength (7d)** — {leader} leading ({eth['r7']:+.1f}% ETH vs {btc['r7']:+.1f}% BTC)",
        f"- **Volatility** — BTC {_vol_str(btc['vol'])} · ETH {_vol_str(eth['vol'])}",
        f"- **Watch tomorrow** — BTC: {_levels_str(btc['levels'])} · ETH: {_levels_str(eth['levels'])}",
        f"- **Paper idea (not live)** — BTC: {bi}",
        f"  - ETH: {ei}",
        f"- **Reason for no action** — BTC: {br}  ETH: {er}",
    ]


def append_journal(now, btc, eth, leader, alerts):
    lines = [f"\n## {now} (auto)"] + build_lesson(btc, eth, leader)
    lines.append("- **⚠️ ALERT:** " + "; ".join(alerts) if alerts else "- _nothing notable_")
    lines.append("- **Review (fill later):** _did the watched level break/hold? forward 1–3d move vs this read?_")
    block = "\n".join(lines) + "\n"
    text = JOURNAL.read_text() if JOURNAL.exists() else "# Trade Journal\n\n# Daily Observations (live cadence)\n"
    marker = "<!-- Next daily read appends here"
    if marker in text:
        i = text.index(marker)
        text = text[:i] + block + "\n" + text[i:]
    else:
        text += block
    JOURNAL.write_text(text)


def append_history(date, btc, eth, leader):
    """Persist a per-day snapshot for the weekly review. Idempotent per date (a re-run
    on the same day replaces that day's record rather than duplicating it)."""
    rec = {"date": date, "leader": leader}
    for nm, d in (("btc", btc), ("eth", eth)):
        lv = d.get("levels") or {}
        rec[nm] = {"price": d["price"], "regime": d["regime"],
                   "support": lv.get("support"), "resistance": lv.get("resistance"),
                   "ma20": lv.get("ma20"),
                   "vol": (d["vol"]["daily_pct"] if d.get("vol") else None)}
    existing = []
    if HISTORY.exists():
        for line in HISTORY.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except Exception:
                continue
            if o.get("date") != date:
                existing.append(o)
    existing.append(rec)
    HISTORY.write_text("\n".join(json.dumps(o) for o in existing) + "\n")


def main():
    ts = datetime.datetime.now()
    now = ts.strftime("%Y-%m-%d %H:%M")
    today = ts.strftime("%Y-%m-%d")

    lock = acquire_lock()  # noqa: F841 — held for the whole run; released on exit
    if lock is None:
        print(f"[{now}] observe: another run holds the lock, skipping")
        return

    s = get_settings()
    cb = CoinbaseService(s, RiskManager(s))
    al = AlpacaService(s, RiskManager(s))
    try:
        btc = read_symbol(cb, al, "BTC-USD", "BTC/USD")
        eth = read_symbol(cb, al, "ETH-USD", "ETH/USD")
    except Exception as e:  # never crash the scheduled job
        print(f"[{now}] observe ERROR: {type(e).__name__}: {e}")
        return
    if not (btc["price"] and eth["price"]):
        print(f"[{now}] observe: incomplete data, skipping")
        return

    leader = "ETH" if (eth["r7"] or 0) > (btc["r7"] or 0) else "BTC"
    prior = {}
    if STATE.exists():
        try:
            prior = json.loads(STATE.read_text())
        except Exception:
            prior = {}
    alerts = detect_alerts(btc, eth, leader, prior)

    append_journal(now, btc, eth, leader, alerts)
    append_history(today, btc, eth, leader)
    STATE.write_text(json.dumps({"ts": now, "btc": btc, "eth": eth, "leader": leader}, indent=2, default=str))

    summary = (f"[{now}] BTC ${btc['price']:,.0f} ({REGIME_LABEL.get(btc['regime'], btc['regime'])}) "
               f"ETH ${eth['price']:,.0f} ({REGIME_LABEL.get(eth['regime'], eth['regime'])}) leader {leader}")
    if alerts:
        summary += " | ALERTS: " + "; ".join(alerts)
        notify("Crypto watch", "; ".join(alerts))
    else:
        summary += " | nothing notable"
    print(summary)


if __name__ == "__main__":
    main()
