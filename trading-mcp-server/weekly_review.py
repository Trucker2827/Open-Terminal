#!/usr/bin/env python3
"""Weekly review of the daily observations — READ-ONLY, no trading, no LLM.

Reads observe_history.jsonl (written by daily_observe.py), forward-tests the regime
signals SYMMETRICALLY (every signal the rule fired, scored net of fees — wins AND
losses AND false breakouts), and appends a "Week ending … (auto review)" block to
trade-journal.md. The honest, expected verdict is usually "no demonstrated edge" —
that is the lesson, not a failure of the script. Degrades gracefully when history is
short (it starts empty and fills one line per day). Shares observe.lock with the
daily observer so a weekly run can't interleave with the 08:57 daily write.
"""
import datetime
import fcntl
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from trading_mcp.thesis import score_history, WEEKLY_HORIZON, WEEKLY_FEE_BPS  # noqa: E402

HISTORY = HERE / "observe_history.jsonl"
JOURNAL = HERE / "trade-journal.md"
LOCK = HERE / "observe.lock"          # shared with daily_observe.py
WINDOW_DAYS = 7


def acquire_lock():
    f = open(LOCK, "w")
    try:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


def load_history():
    if not HISTORY.exists():
        return []
    out = []
    for line in HISTORY.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except Exception:
            continue
    out.sort(key=lambda o: o.get("date", ""))
    return out


def regime_counts(history, asset):
    counts = {}
    for h in history:
        r = (h.get(asset) or {}).get("regime")
        if r:
            counts[r] = counts.get(r, 0) + 1
    return counts


def fmt_counts(counts):
    return ", ".join(f"{k}×{v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])) or "none"


def fmt_score(s):
    if s["n_signals"] == 0:
        return f"0 directional signals over {s['days']} days — nothing to score yet"
    hit = f"{s['hit_rate'] * 100:.0f}%" if s["hit_rate"] is not None else "n/a"
    exp = f"{s['expectancy'] * 100:+.2f}%" if s["expectancy"] is not None else "n/a"
    return (f"{s['n_signals']} signals, hit {hit}, expectancy {exp}/trade net of fees, "
            f"{s['false_breakouts']} false breakout(s) → {s['verdict']}")


def build_block(history):
    n = len(history)
    week = history[-WINDOW_DAYS:] if n > WINDOW_DAYS else history
    span = (f"{week[0]['date']}…{week[-1]['date']}" if week else "no data")
    btc_s = score_history(history, "btc")   # score the full history, not just the window
    eth_s = score_history(history, "eth")
    leader_days = sum(1 for h in week if h.get("leader") == "ETH")
    lines = [
        f"\n## Week review {datetime.datetime.now():%Y-%m-%d} (auto, READ-ONLY)",
        f"- **Observed** — {n} day(s) of history total ({span} shown). "
        f"BTC regimes: {fmt_counts(regime_counts(week, 'btc'))} · "
        f"ETH regimes: {fmt_counts(regime_counts(week, 'eth'))} · ETH led {leader_days}/{len(week)} days.",
        f"- **What would have worked (symmetric, {WEEKLY_HORIZON}d horizon, {WEEKLY_FEE_BPS:.0f}bps/side)** — "
        f"BTC: {fmt_score(btc_s)}",
        f"  - ETH: {fmt_score(eth_s)}",
        f"- **Net lesson** — {_net_lesson(btc_s, eth_s)}",
        "- **Next** — keep observing; only revisit arming *paper* (never live) if expectancy turns "
        "convincingly positive over many more signals. Live stays a human GUI toggle.",
    ]
    return "\n".join(lines) + "\n"


def _net_lesson(btc_s, eth_s):
    total = btc_s["n_signals"] + eth_s["n_signals"]
    if total < 5:
        return (f"Data-starved: only {total} directional signal(s) so far. No conclusion yet — "
                "history fills one line per day; revisit after ~2–4 weeks.")
    edge = [s for s in (btc_s, eth_s) if (s["expectancy"] or 0) > 0 and s["n_signals"] >= 5]
    if not edge:
        return ("No demonstrated edge across either asset, net of fees — consistent with the "
                "MA-crossover backtests. The honest call remains: stay flat / paper only.")
    names = "/".join("BTC" if s is btc_s else "ETH" for s in edge)
    return (f"{names} shows positive net expectancy — still PAPER only, treat as a hypothesis to keep "
            "forward-testing, not a green light.")


def append_to_journal(block):
    text = JOURNAL.read_text() if JOURNAL.exists() else "# Trade Journal\n\n# Daily Observations (live cadence)\n"
    marker = "<!-- Next daily read appends here"
    if marker in text:
        i = text.index(marker)
        text = text[:i] + block + "\n" + text[i:]
    else:
        text += block
    JOURNAL.write_text(text)


def main():
    lock = acquire_lock()  # noqa: F841 — held until process exit
    if lock is None:
        print("weekly_review: another observe run holds the lock, skipping")
        return
    history = load_history()
    if not history:
        print("weekly_review: no history yet (observe_history.jsonl empty) — nothing to review")
        return
    block = build_block(history)
    append_to_journal(block)
    print(f"weekly_review: appended review for {len(history)} day(s) of history")
    print(block.strip())


if __name__ == "__main__":
    main()
