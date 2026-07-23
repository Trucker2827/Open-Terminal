#!/usr/bin/env python3
"""Alpha Arena leaderboard report.

Mechanical, preregistered, honest:
  - Brier (blind commitments vs settlements) comes from the terminal's own
    `advise score` verb, filtered per lane by provider+model.
  - Coverage comes from the arena round journal: committed / offered. A lane
    below the coverage floor is NEVER ranked — it is listed NOT_COMPARABLE
    with the reason (the v4 duel lesson: timeouts select easy cases).
  - No lane, no round, no number is fabricated. Missing data reads as
    missing.

Seasons (P3): `season open '{"days":14,"min_resolved":300}'` writes an
immutable, sealed season file; once open its parameters cannot change
(`season open` refuses while a season is open, the file is written
read-only, and every reader re-verifies the seal hash). While a season
file exists, the report's coverage window and verdict thresholds come
ONLY from that season: rounds outside the window are excluded and the
CLI Brier is windowed via `advise score --since-ms/--until-ms`.

Evidence paths (all under the arena evidence directory):
  arena-report.json        leaderboard report (this script's default out)
  arena-season.json        the current sealed season (season open)
  arena-season-<id>.json   archived closed seasons (never deleted)
  arena-leaderboard.html   self-contained public export (export-html)

Usage: arena_report.py [--min-coverage 0.8] [--min-resolved 50] [--out PATH]
       arena_report.py season open '{"days":14,"min_resolved":300}'
       arena_report.py season status
       arena_report.py export-html [--report PATH] [--html-out PATH]
"""
import argparse
import hashlib
import html
import json
import os
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "kalshi_advise")))

from arena_loop import EVIDENCE_DIR, ROUNDS_PATH, load_registry
from advise_challenge import DEFAULT_CLI

DEFAULT_OUT = os.path.join(EVIDENCE_DIR, "arena-report.json")
SEASON_PATH = os.path.join(EVIDENCE_DIR, "arena-season.json")
HTML_PATH = os.path.join(EVIDENCE_DIR, "arena-leaderboard.html")

SEASON_PARAM_KEYS = {"days", "min_resolved", "min_coverage"}
DEFAULT_MIN_COVERAGE = 0.80

HOW_IT_WORKS = (
    "Every model sees the same facts at the same instant — never the market's "
    "odds. Each seals its probability before anyone can peek. Kalshi settles "
    "the truth within the hour. Lower Brier = better forecaster. A model that "
    "skips too many rounds (coverage below the floor) is not ranked — skipping "
    "the hard ones must not look like skill.")


def read_rounds(path=ROUNDS_PATH):
    rounds = []
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    try:
                        rounds.append(json.loads(line))
                    except ValueError:
                        continue
    except OSError:
        pass
    return rounds


def coverage_from_rounds(rounds):
    """{lane_id: {"offered": n, "committed": n, "abstained": n, "expired": n}}"""
    stats = {}
    for rnd in rounds:
        if rnd.get("status") != "DONE":
            continue
        for lane in rnd.get("lanes", []):
            s = stats.setdefault(lane.get("id"), {"offered": 0, "committed": 0,
                                                  "abstained": 0, "expired": 0})
            s["offered"] += 1
            status = lane.get("status")
            if status == "COMMITTED_BLIND":
                s["committed"] += 1
            elif status == "ABSTAINED":
                s["abstained"] += 1
            elif status == "EXPIRED":
                s["expired"] += 1
    return stats


def season_seal(record):
    """Seal hash over every field except the seal itself — any post-open edit
    to the preregistered parameters (or the window they derive) breaks it."""
    sealed = {k: v for k, v in record.items() if k != "seal_sha256"}
    canon = json.dumps(sealed, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()


def parse_season_params(raw):
    try:
        params = json.loads(raw)
    except ValueError:
        raise ValueError(f"season params must be JSON: {raw!r}")
    if not isinstance(params, dict):
        raise ValueError("season params must be a JSON object")
    unknown = set(params) - SEASON_PARAM_KEYS
    if unknown:
        raise ValueError(f"unknown season params {sorted(unknown)}; "
                         f"allowed: {sorted(SEASON_PARAM_KEYS)}")
    days = params.get("days")
    if not isinstance(days, (int, float)) or isinstance(days, bool) or days <= 0:
        raise ValueError("season param 'days' must be a number > 0")
    min_resolved = params.get("min_resolved")
    if not isinstance(min_resolved, int) or isinstance(min_resolved, bool) \
            or min_resolved <= 0:
        raise ValueError("season param 'min_resolved' must be an integer > 0")
    min_coverage = params.get("min_coverage", DEFAULT_MIN_COVERAGE)
    if not isinstance(min_coverage, (int, float)) or isinstance(min_coverage, bool) \
            or not 0.0 < min_coverage <= 1.0:
        raise ValueError("season param 'min_coverage' must be in (0, 1]")
    return {"days": days, "min_resolved": min_resolved,
            "min_coverage": float(min_coverage)}


def load_season(path=SEASON_PATH):
    """The current season record, or None. Raises ValueError if the sealed
    file was altered after opening — a broken seal is never read as data."""
    try:
        with open(path, encoding="utf-8") as fh:
            record = json.load(fh)
    except OSError:
        return None
    except ValueError:
        raise ValueError(f"season file {path} is not valid JSON — refusing to guess")
    if record.get("seal_sha256") != season_seal(record):
        raise ValueError(f"season file {path} failed its seal check — the "
                         "preregistered parameters were altered after opening")
    return record


def season_state(season, now_ms):
    return "OPEN" if now_ms < season["closes_at_ms"] else "CLOSED"


def open_season(raw_params, now_ms, path=SEASON_PATH):
    """Open a new season with preregistered, immutable parameters. Refuses
    while a season is open; archives (never deletes) a closed predecessor."""
    params = parse_season_params(raw_params)
    existing = load_season(path)
    if existing and season_state(existing, now_ms) == "OPEN":
        raise ValueError(
            f"season {existing['season_id']} is open until "
            f"{existing['closes_at_ms']} ms — parameters are immutable; "
            "wait for it to close before opening a new season")
    if existing:
        archive = os.path.join(os.path.dirname(path),
                               f"arena-season-{existing['season_id']}.json")
        os.replace(path, archive)
    record = {"schema": 1, "event": "arena_season",
              "season_id": str(uuid.uuid4()),
              "opened_at_ms": now_ms,
              "closes_at_ms": now_ms + int(params["days"] * 86400000),
              "params": params}
    record["seal_sha256"] = season_seal(record)
    write_atomic(path, json.dumps(record, indent=2))
    os.chmod(path, 0o444)
    return record


def round_in_window(rnd, season):
    ts = rnd.get("opened_at_ms")
    return (isinstance(ts, (int, float))
            and season["opened_at_ms"] <= ts < season["closes_at_ms"])


def season_summary(season, now_ms):
    return {"season_id": season["season_id"], "params": season["params"],
            "opened_at_ms": season["opened_at_ms"],
            "closes_at_ms": season["closes_at_ms"],
            "state": season_state(season, now_ms)}


def season_status(season, rounds, now_ms):
    """Journal-side season progress. Resolved counts live in the terminal DB
    and appear in the full report — they are not fabricated here."""
    length_ms = season["closes_at_ms"] - season["opened_at_ms"]
    elapsed_ms = min(max(now_ms - season["opened_at_ms"], 0), length_ms)
    in_window = [r for r in rounds if round_in_window(r, season)]
    coverage = coverage_from_rounds(in_window)
    out = season_summary(season, now_ms)
    out.update({
        "schema": 1, "event": "arena_season_status", "now_ms": now_ms,
        "elapsed_days": round(elapsed_ms / 86400000.0, 2),
        "remaining_days": round((length_ms - elapsed_ms) / 86400000.0, 2),
        "rounds_done_in_window": sum(1 for r in in_window
                                     if r.get("status") == "DONE"),
        "committed_in_window": sum(s["committed"] for s in coverage.values()),
        "lanes": coverage,
        "min_resolved_target": season["params"]["min_resolved"],
        "note": "resolved counts come from the leaderboard report, not this status",
    })
    return out


def cli_score(lane, cli=DEFAULT_CLI, profile=None, since_ms=None, until_ms=None):
    """Per-lane Brier via the terminal's advise score verb. None on failure."""
    cmd = [cli, "--json", "--headless"]
    if profile:
        cmd += ["--profile", profile]
    cmd += ["kalshi", "auto", "advise", "score",
            "--provider", f"arena-{lane['kind']}", "--model", lane["model"]]
    if since_ms is not None:
        cmd += ["--since-ms", str(int(since_ms))]
    if until_ms is not None:
        cmd += ["--until-ms", str(int(until_ms))]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        return json.loads(r.stdout) if r.stdout.strip() else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def build_report(lanes, rounds, score_fn, min_coverage, min_resolved, now_ms,
                 season=None):
    if season:
        rounds = [r for r in rounds if round_in_window(r, season)]
        min_coverage = season["params"]["min_coverage"]
        min_resolved = season["params"]["min_resolved"]
    coverage = coverage_from_rounds(rounds)
    entries = []
    for lane in lanes:
        cov = coverage.get(lane["id"], {"offered": 0, "committed": 0,
                                        "abstained": 0, "expired": 0})
        offered = cov["offered"]
        cov_ratio = (cov["committed"] / offered) if offered else None
        score = score_fn(lane) or {}
        resolved = ((score.get("coverage") or {}).get("resolved")
                    if isinstance(score.get("coverage"), dict) else 0) or 0
        entry = {
            "id": lane["id"], "model": lane["model"], "epoch_id": lane["epoch_id"],
            "offered": offered, "committed": cov["committed"],
            "abstained": cov["abstained"], "expired": cov["expired"],
            "coverage": cov_ratio, "resolved": resolved,
            "brier": score.get("brier_pre"),
            "brier_ci": [score.get("ci_low"), score.get("ci_high")],
        }
        if offered == 0:
            entry.update(comparable=False, reason="no rounds offered yet")
        elif cov_ratio is not None and cov_ratio < min_coverage:
            entry.update(comparable=False,
                         reason=f"coverage {cov_ratio:.0%} below {min_coverage:.0%} floor")
        elif resolved < min_resolved:
            entry.update(comparable=False,
                         reason=f"only {resolved}/{min_resolved} commitments resolved")
        else:
            entry.update(comparable=True, reason=None)
        entries.append(entry)

    ranked = sorted([e for e in entries if e["comparable"]],
                    key=lambda e: (e["brier"] if e["brier"] is not None else 9.9))
    for pos, e in enumerate(ranked, start=1):
        e["rank"] = pos
    verdict = "INSUFFICIENT_DATA"
    if len(ranked) >= 2:
        top, second = ranked[0], ranked[1]
        top_hi = top["brier_ci"][1]
        second_lo = second["brier_ci"][0]
        if (top["brier"] is not None and second["brier"] is not None
                and top_hi is not None and second_lo is not None
                and top_hi < second_lo):
            verdict = f"LEADER: {top['id']}"
        else:
            verdict = "STATISTICAL_TIE_SO_FAR"
    return {
        "schema": 1, "event": "arena_report", "advisory_only": True,
        "generated_at_ms": now_ms,
        "how_it_works": HOW_IT_WORKS,
        "thresholds": {"min_coverage": min_coverage, "min_resolved": min_resolved},
        "rounds_total": sum(1 for r in rounds if r.get("status") == "DONE"),
        "season": season_summary(season, now_ms) if season else None,
        "verdict": verdict,
        "leaderboard": entries,
    }


def _esc(value):
    return html.escape("—" if value is None else str(value), quote=True)


def _fmt(value, spec):
    return format(value, spec) if isinstance(value, (int, float)) else "—"


def render_html(report):
    """Self-contained leaderboard page: inline CSS only, no external assets,
    every dynamic string escaped. Missing values render as an em dash."""
    rows = []
    for e in report.get("leaderboard", []):
        status = f"#{e['rank']}" if e.get("comparable") else \
            f"not ranked — {e.get('reason') or 'no data'}"
        ci = e.get("brier_ci") or [None, None]
        ci_txt = (f"[{_fmt(ci[0], '.4f')}, {_fmt(ci[1], '.4f')}]"
                  if ci[0] is not None and ci[1] is not None else "—")
        cls = "ranked" if e.get("comparable") else "unranked"
        rows.append(
            f'<tr class="{cls}"><td>{_esc(status)}</td><td>{_esc(e.get("id"))}</td>'
            f'<td>{_esc(e.get("model"))}</td><td>{_fmt(e.get("brier"), ".4f")}</td>'
            f"<td>{ci_txt}</td><td>{_fmt(e.get('coverage'), '.0%')}</td>"
            f"<td>{_fmt(e.get('committed'), 'd')}/{_fmt(e.get('offered'), 'd')}</td>"
            f"<td>{_fmt(e.get('resolved'), 'd')}</td></tr>")
    season = report.get("season")
    season_line = ""
    if season:
        p = season.get("params", {})
        season_line = (
            f"<p class=\"season\">Season {_esc(season.get('season_id'))} "
            f"({_esc(season.get('state'))}): {_esc(p.get('days'))} days, "
            f"min resolved {_esc(p.get('min_resolved'))}, "
            f"coverage floor {_fmt(p.get('min_coverage'), '.0%')}</p>")
    generated = report.get("generated_at_ms")
    generated_txt = (time.strftime("%Y-%m-%d %H:%M:%S UTC",
                                   time.gmtime(generated / 1000.0))
                     if isinstance(generated, (int, float)) else "—")
    thresholds = report.get("thresholds", {})
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Alpha Arena — Leaderboard</title>
<style>
body {{ font-family: -apple-system, "Segoe UI", sans-serif; margin: 2rem auto;
       max-width: 60rem; padding: 0 1rem; background: #101418; color: #e8e8e8; }}
h1 {{ font-size: 1.4rem; }}
.verdict {{ font-size: 1.1rem; padding: .6rem 1rem; background: #1b2733;
            border-left: 4px solid #4a9; }}
.season {{ color: #9ab; }}
table {{ border-collapse: collapse; width: 100%; margin-top: 1rem; }}
th, td {{ text-align: left; padding: .4rem .6rem; border-bottom: 1px solid #2a3540; }}
tr.unranked td {{ color: #778; }}
.how {{ color: #9ab; font-size: .9rem; margin-top: 1.5rem; }}
.meta {{ color: #667; font-size: .8rem; }}
</style></head><body>
<h1>Alpha Arena — Leaderboard</h1>
<p class="verdict">{_esc(report.get("verdict"))}</p>
{season_line}
<p class="meta">Generated {_esc(generated_txt)} ·
rounds: {_esc(report.get("rounds_total"))} ·
coverage floor {_fmt(thresholds.get("min_coverage"), ".0%")} ·
min resolved {_esc(thresholds.get("min_resolved"))} · advisory only</p>
<table><thead><tr><th>Rank</th><th>Lane</th><th>Model</th><th>Brier</th>
<th>95% CI</th><th>Coverage</th><th>Committed</th><th>Resolved</th></tr></thead>
<tbody>{"".join(rows)}</tbody></table>
<p class="how">{_esc(report.get("how_it_works"))}</p>
<p class="meta">Read-only evidence export of arena-report.json — nothing on
this page can place orders.</p>
</body></html>
"""


def write_atomic(path, text):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def season_command(argv, now_ms):
    if argv[:1] == ["open"]:
        if len(argv) != 2:
            print("usage: arena_report.py season open '<params json>'", file=sys.stderr)
            return 2
        record = open_season(argv[1], now_ms)
        print(json.dumps({"out": SEASON_PATH, "season": season_summary(record, now_ms)}))
        return 0
    if argv == ["status"]:
        season = load_season()
        if season is None:
            print(json.dumps({"error": "no season file", "path": SEASON_PATH}),
                  file=sys.stderr)
            return 1
        print(json.dumps(season_status(season, read_rounds(), now_ms), indent=2))
        return 0
    print("usage: arena_report.py season open '<params json>' | season status",
          file=sys.stderr)
    return 2


def export_html_command(report_path, out_path):
    try:
        with open(report_path, encoding="utf-8") as fh:
            report = json.load(fh)
    except (OSError, ValueError) as exc:
        print(json.dumps({"error": f"cannot read report {report_path}: {exc}",
                          "hint": "run arena_report.py first"}), file=sys.stderr)
        return 1
    write_atomic(out_path, render_html(report))
    print(json.dumps({"out": out_path, "verdict": report.get("verdict")}))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Alpha Arena leaderboard report")
    ap.add_argument("command", nargs="*",
                    help="season open '<json>' | season status | export-html "
                         "(default: write the leaderboard report)")
    ap.add_argument("--min-coverage", type=float, default=DEFAULT_MIN_COVERAGE)
    ap.add_argument("--min-resolved", type=int, default=50)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--report", default=DEFAULT_OUT)
    ap.add_argument("--html-out", default=HTML_PATH)
    ap.add_argument("--cli", default=DEFAULT_CLI)
    ap.add_argument("--profile", default=None)
    args = ap.parse_args()
    now_ms = int(time.time() * 1000)

    try:
        if args.command[:1] == ["season"]:
            return season_command(args.command[1:], now_ms)
        if args.command == ["export-html"]:
            return export_html_command(args.report, args.html_out)
        if args.command:
            print(f"unknown command {args.command!r}", file=sys.stderr)
            return 2
        season = load_season()
    except ValueError as exc:
        print(json.dumps({"error": str(exc)}), file=sys.stderr)
        return 1

    since_ms = season["opened_at_ms"] if season else None
    until_ms = season["closes_at_ms"] if season else None
    lanes = load_registry()
    rounds = read_rounds()
    report = build_report(lanes, rounds,
                          lambda lane: cli_score(lane, args.cli, args.profile,
                                                 since_ms=since_ms,
                                                 until_ms=until_ms),
                          args.min_coverage, args.min_resolved, now_ms,
                          season=season)
    write_atomic(args.out, json.dumps(report, indent=2))
    print(json.dumps({"out": args.out, "verdict": report["verdict"],
                      "rounds": report["rounds_total"],
                      "season": report["season"]["season_id"] if report["season"] else None,
                      "lanes": [e["id"] for e in report["leaderboard"]]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
