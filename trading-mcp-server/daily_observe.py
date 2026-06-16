#!/usr/bin/env python3
"""Headless daily BTC/ETH observation — OBSERVATION ONLY (never trades).

Run by OS cron. Pulls live data via the trading_mcp READ paths only, appends a
dated entry to trade-journal.md, detects key-level breaks / relative-strength
flips / large moves vs the prior run (state in observe_state.json), and fires a
macOS desktop notification on any alert. No LLM, no order placement, no settings
changes — it imports the read tools and calls only get_product / get_historical_data.
"""
import datetime
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)  # so .env and the trading_mcp package resolve regardless of cron's cwd
sys.path.insert(0, str(HERE))

from trading_mcp.config import get_settings          # noqa: E402
from trading_mcp.safety import RiskManager            # noqa: E402
from trading_mcp.coinbase_tools import CoinbaseService  # noqa: E402
from trading_mcp.alpaca_tools import AlpacaService    # noqa: E402

STATE = HERE / "observe_state.json"
JOURNAL = HERE / "trade-journal.md"
ALERT_24H_PCT = 8.0


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def read_symbol(cb, al, cb_sym, al_sym):
    prod = cb.client.get_product(cb_sym)
    d = prod.to_dict() if hasattr(prod, "to_dict") else (prod if isinstance(prod, dict) else {})
    price = num(d.get("price"))
    chg24 = num(d.get("price_percentage_change_24h"))
    hd = al.get_historical_data(al_sym, "1Day", "crypto", 30)
    raw = hd.get("bars", {})
    bars = (raw.get("data", {}) or {}).get(al_sym) if isinstance(raw, dict) else None
    if not bars and isinstance(raw, dict):
        bars = raw.get(al_sym)
    closes = [num(b.get("close")) for b in (bars or []) if b.get("close") is not None]
    hi30 = max(closes) if closes else None
    lo30 = min(closes) if closes else None
    pos = (price - lo30) / (hi30 - lo30) * 100 if (price and hi30 and lo30 and hi30 > lo30) else None
    r7 = (price / closes[-7] - 1) * 100 if (price and len(closes) >= 7) else None
    return {"price": price, "chg24": chg24, "hi30": hi30, "lo30": lo30, "pos": pos, "r7": r7}


def notify(title, msg):
    # Best-effort macOS notification. From cron it may not reach the Aqua session;
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


def journal_line(nm, d):
    return (f"- **{nm}** ${d['price']:,.2f} · 24h {d['chg24']:+.2f}% · 7d {d['r7']:+.1f}% · "
            f"30d ${d['lo30']:,.0f}–${d['hi30']:,.0f} · at {d['pos']:.0f}% of range")


def append_journal(now, btc, eth, leader, alerts):
    lines = [f"\n## {now} (auto)", journal_line("BTC", btc), journal_line("ETH", eth),
             f"- **Relative (7d):** {leader} leading ({eth['r7']:+.1f}% ETH vs {btc['r7']:+.1f}% BTC)"]
    lines.append("- **⚠️ ALERT:** " + "; ".join(alerts) if alerts else "- _nothing notable_")
    lines.append(f"- **Levels to watch:** BTC ${btc['lo30']:,.0f}/${btc['hi30']:,.0f} · "
                 f"ETH ${eth['lo30']:,.0f}/${eth['hi30']:,.0f}")
    block = "\n".join(lines) + "\n"
    text = JOURNAL.read_text() if JOURNAL.exists() else "# Trade Journal\n\n# Daily Observations (live cadence)\n"
    marker = "<!-- Next daily read appends here"
    if marker in text:
        i = text.index(marker)
        text = text[:i] + block + "\n" + text[i:]
    else:
        text += block
    JOURNAL.write_text(text)


def main():
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    s = get_settings()
    cb = CoinbaseService(s, RiskManager(s))
    al = AlpacaService(s, RiskManager(s))
    try:
        btc = read_symbol(cb, al, "BTC-USD", "BTC/USD")
        eth = read_symbol(cb, al, "ETH-USD", "ETH/USD")
    except Exception as e:  # never crash the cron job
        print(f"[{now}] observe ERROR: {type(e).__name__}: {e}")
        sys.exit(0)
    if not (btc["price"] and eth["price"]):
        print(f"[{now}] observe: incomplete data, skipping")
        sys.exit(0)

    leader = "ETH" if (eth["r7"] or 0) > (btc["r7"] or 0) else "BTC"
    prior = {}
    if STATE.exists():
        try:
            prior = json.loads(STATE.read_text())
        except Exception:
            prior = {}
    alerts = detect_alerts(btc, eth, leader, prior)

    append_journal(now, btc, eth, leader, alerts)
    STATE.write_text(json.dumps({"ts": now, "btc": btc, "eth": eth, "leader": leader}, indent=2))

    summary = f"[{now}] BTC ${btc['price']:,.0f} ETH ${eth['price']:,.0f} leader {leader}"
    if alerts:
        summary += " | ALERTS: " + "; ".join(alerts)
        notify("Crypto watch", "; ".join(alerts))
    else:
        summary += " | nothing notable"
    print(summary)


if __name__ == "__main__":
    main()
