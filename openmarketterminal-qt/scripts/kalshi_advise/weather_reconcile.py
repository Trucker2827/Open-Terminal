#!/usr/bin/env python3
"""Weather settlement reconciler — closes the loop the producer never did.

The weather producer writes gate='pass' decisions to edge_decision_journal
(source='kalshi weather-plan') with outcome=-1 (unknown at decision time).
SandboxResolver::resolve_predictions settles an open paper position ONLY when
its journal row's outcome is 0/1 — but nothing ever wrote weather outcomes
(the crypto path does this via KalshiScreen::reconcile_settlement). So weather
positions never settled. This job fetches settled KXHIGH* markets from Kalshi
and writes outcome (1=yes, 0=no) into the matching journal rows; the executor's
resolve_predictions then closes the positions on its next cycle.

Dry-run by default (prints what WOULD resolve, writes nothing). --write applies.
Low-risk: only sets outcome for journal rows whose Kalshi market has actually
settled, using Kalshi's own result.
"""
import sys, os, json, time, argparse, urllib.request, sqlite3, subprocess
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from openterminal_paths import journal_db  # noqa: E402

BASE = "https://external-api.kalshi.com/trade-api/v2"
SOURCE = "kalshi weather-plan"

def http_json(url, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=25) as r:
                return json.load(r)
        except Exception:
            if i == tries - 1:
                return None
            time.sleep(0.4)

def settled_results(series):
    """ticker -> 'yes'/'no' for all settled markets in a series (paginated)."""
    out, cursor = {}, ""
    for _ in range(20):
        u = f"{BASE}/markets?series_ticker={series}&status=settled&limit=200" + (f"&cursor={cursor}" if cursor else "")
        d = http_json(u)
        if not d:
            break
        for m in d.get("markets", []):
            res = m.get("result")
            if res in ("yes", "no"):
                out[m["ticker"]] = res
        cursor = d.get("cursor") or ""
        if not cursor:
            break
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="apply outcomes (default: dry run)")
    args = ap.parse_args()

    con = sqlite3.connect(journal_db())
    cur = con.cursor()
    rows = cur.execute(
        "SELECT id, market_id FROM edge_decision_journal "
        "WHERE source=? AND outcome=-1", (SOURCE,)).fetchall()
    if not rows:
        print("no unresolved weather journal rows.")
        return

    # group unresolved market_ids by series (prefix before first '-')
    series_set = sorted({mid.split("-", 1)[0] for _, mid in rows})
    print(f"unresolved rows: {len(rows)} across {len(set(m for _, m in rows))} markets, "
          f"series: {', '.join(series_set)}", file=sys.stderr)

    results = {}
    for s in series_set:
        results.update(settled_results(s))
    print(f"fetched {len(results)} settled Kalshi markets across those series", file=sys.stderr)

    now_ms = int(time.time() * 1000)
    to_update, still_pending = [], 0
    for jid, mid in rows:
        res = results.get(mid)
        if res is None:
            still_pending += 1           # market not settled on Kalshi yet (e.g., today's)
            continue
        to_update.append((jid, mid, 1 if res == "yes" else 0, res))

    print(f"\n=== WEATHER RECONCILE ({'WRITE' if args.write else 'DRY RUN'}) ===")
    print(f"resolvable now: {len(to_update)}   still-pending (Kalshi unsettled): {still_pending}")
    for jid, mid, oc, res in to_update[:20]:
        print(f"  {mid:26} -> outcome={oc} ({res})")
    if len(to_update) > 20:
        print(f"  ... +{len(to_update) - 20} more")

    if args.write and to_update:
        cur.executemany(
            "UPDATE edge_decision_journal SET outcome=?, resolved_at=?, updated_at=? WHERE id=?",
            [(oc, now_ms, now_ms, jid) for jid, _, oc, _ in to_update])
        con.commit()
        print(f"\nWROTE {len(to_update)} outcomes.")
        # Settle now with the freshly-built cli (paper-only run_cycle -> side-aware
        # resolve_predictions). Self-contained so settlement never depends on a
        # stale-binary persistent executor. No live orders: sandbox tick is paper.
        cli = "/Users/haydarevich/src/Open-Terminal/openmarketterminal-qt/build/openterminalcli"
        if os.path.exists(cli):
            try:
                r = subprocess.run([cli, "--profile", "default", "sandbox", "tick"],
                                   timeout=120, capture_output=True, text=True)
                print(f"settled via fixed cli (exit {r.returncode}).")
            except Exception as e:
                print(f"tick failed: {e} (positions will settle on next executor cycle)")
    elif not args.write:
        print("\n(dry run — nothing written; re-run with --write to apply)")
    con.close()

if __name__ == "__main__":
    main()
