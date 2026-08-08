#!/usr/bin/env python3
"""Per-settlement post-mortem for the paper Kalshi bot.

W/L counts are not enough. Every paper settlement is joined back to the
originating bid (`position_id`) and classified so losses (especially) can
teach the next bid — what was paid, how much runway remained, whether the
bot crossed, and why it could not cut the loss after fill.

Always writes two cohorts (unless `--no-write`):

  Historic (learning + sealed-gate ledger):
    kalshi-bot-postmortems.jsonl
    kalshi-bot-postmortem-summary.json

  Current rules (score new trading fairly; post-gate / mid_path window):
    kalshi-bot-postmortems-current.jsonl
    kalshi-bot-postmortem-summary-current.json

Do not judge new decide/cashout rules by pre-gate lifetime losses.
Human summary on stdout by default; `--json` for machine summary.

  python3 scripts/kalshi_advise/bot_bid_postmortem.py
  python3 scripts/kalshi_advise/bot_bid_postmortem.py --json
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
from openterminal_paths import evidence_dir  # noqa: E402

LEDGER_GLOB = "kalshi-bot-decisions.jsonl*"
POSTMORTEM_JSONL = "kalshi-bot-postmortems.jsonl"
SUMMARY_JSON = "kalshi-bot-postmortem-summary.json"
CURRENT_POSTMORTEM_JSONL = "kalshi-bot-postmortems-current.jsonl"
CURRENT_SUMMARY_JSON = "kalshi-bot-postmortem-summary-current.json"

# Hold-to-settle losses: cashout exists but did not fire (HOLD / missing bid).
HELD_TO_SETTLE = (
    "held to settlement: paper cashout did not fire (HOLD_EDGE_INTACT, missing "
    "held-side bid, or stale report) — cancels still only apply to resting quotes"
)


def _evidence() -> Path:
    return Path(evidence_dir())


def _load_ledger() -> Tuple[List[dict], List[str]]:
    root = _evidence()
    paths = sorted(root.glob(LEDGER_GLOB), key=lambda p: (0 if p.name.endswith(".jsonl") else 1, p.name))
    # Prefer chronological: archived generations then live. Generation numbers
    # ascend with age (.5 oldest-ish); live file is newest activity but also
    # holds the current generation — read all, sort rows by ts_ms later.
    records: List[dict] = []
    files: List[str] = []
    for path in paths:
        files.append(path.name)
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                except ValueError:
                    continue
    return records, files


def _family(ticker: str) -> str:
    head = (ticker or "").split("-", 1)[0]
    if head == "KXBTC15M":
        return "kxbtc15m"
    if head in ("KXGOLD15M", "KXSILVER15M", "KXWTI15M"):
        return "commodities15m"
    return "threshold"


def _payoff_bucket(side: str, price: float, won: bool) -> str:
    """Favourite / longshot label from entry price of the side purchased."""
    # Price is the price paid for `side`. High price = favourite on that side.
    if price >= 0.65:
        kind = "favourite"
    elif price <= 0.35:
        kind = "longshot"
    else:
        kind = "midprice"
    return f"{kind}_{'win' if won else 'loss'}"


def _timing_bucket(runway_seconds: Optional[float]) -> str:
    if runway_seconds is None:
        return "runway_unknown"
    if runway_seconds <= 180:
        return "near_close_le_3m"
    if runway_seconds <= 600:
        return "late_window_le_10m"
    return "earlier_in_window"


def _pick(bid: dict, settlement: dict, key: str) -> Any:
    """Prefer the joined bid row; fall back to settle-time bid snapshot fields."""
    if key in bid and bid.get(key) is not None:
        return bid.get(key)
    return settlement.get(key)


def _gamma_from_mid_path(
    mid_path: Any,
    market_mid: Any,
    won: bool,
) -> Optional[str]:
    """Classify gamma from persisted YES-mid samples (fail closed if thin)."""
    if not isinstance(mid_path, list) or len(mid_path) < 2:
        return None
    mids: List[float] = []
    for sample in mid_path:
        if not isinstance(sample, dict):
            continue
        mid = sample.get("market_yes_mid")
        if isinstance(mid, (int, float)):
            mids.append(float(mid))
    if len(mids) < 2:
        return None
    start = float(market_mid) if isinstance(market_mid, (int, float)) else mids[0]
    end = mids[-1]
    span = max(mids) - min(mids)
    if span < 0.05:
        return "mid_stable_correct" if won else "mid_stable_wrong_thesis"
    early = sum(mids[:-1]) / float(len(mids) - 1)
    # Late move: path was quiet then the last sample jumped ≥10¢.
    if abs(end - early) >= 0.10 and abs(end - start) >= 0.10:
        return "path_late_move"
    return "mid_moved_before_settle"


def _gamma_factor(
    market_mid: Any,
    market_mid_at_settle: Any,
    runway: Any,
    won: bool,
    mid_path: Any = None,
) -> str:
    """Classify late-flip risk without inventing a path.

    Prefer `mid_path` when ≥2 samples exist. Else bid mid + settle mid can
    say stable-wrong vs mid-moved. Near-close runway is a last-resort proxy
    when mids are missing.
    """
    from_path = _gamma_from_mid_path(mid_path, market_mid, won)
    if from_path is not None:
        return from_path
    if isinstance(market_mid, (int, float)) and isinstance(market_mid_at_settle, (int, float)):
        delta = float(market_mid_at_settle) - float(market_mid)
        if abs(delta) < 0.05:
            return "mid_stable_correct" if won else "mid_stable_wrong_thesis"
        if isinstance(runway, (int, float)) and float(runway) <= 180 and abs(delta) >= 0.10:
            return "possible_near_close_flip"
        return "mid_moved_before_settle"
    if isinstance(runway, (int, float)) and float(runway) <= 180:
        return "possible_near_close_noise"
    return "unknown_without_path_replay"


def _classify(settlement: dict, bid: Optional[dict]) -> Dict[str, Any]:
    ticker = settlement.get("ticker") or ""
    side = settlement.get("side") or ""
    result = settlement.get("market_result") or ""
    early_exit = (settlement.get("resolution") or "") == "early_exit"
    won_raw = settlement.get("won")
    won = None if early_exit or won_raw is None else bool(won_raw)
    price = float(settlement.get("price") or 0.0)
    pnl = float(settlement.get("realized_pnl") or 0.0)
    fee = float(settlement.get("fee_usd") or 0.0)
    stake = float(settlement.get("stake_usd") or 0.0)
    contracts = int(settlement.get("contracts") or 0)
    close_reason = settlement.get("close_reason") or ""
    exit_price = settlement.get("exit_price")

    tags: List[str] = []
    if early_exit:
        tags.append("early_exit")
        tags.append("cashout")
        if close_reason:
            tags.append(f"close_{close_reason}")
        tags.append("cashout_win" if pnl > 0 else "cashout_loss" if pnl < 0 else "cashout_flat")
    else:
        tags.append("win" if won else "loss")
        tags.append(_payoff_bucket(side, price, bool(won)))
    tags.append(_family(ticker))

    if (not early_exit) and side and result and side != result:
        tags.append("wrong_side_at_settlement")

    bid = bid or {}
    bid_joined = bool(bid.get("position_id") or bid.get("ts_ms"))
    quote = _pick(bid, settlement, "quote_style") or "unknown_quote"
    tags.append(f"quote_{quote}")

    calibrated_p = _pick(bid, settlement, "calibrated_p")
    market_mid = _pick(bid, settlement, "market_mid")
    market_mid_at_settle = settlement.get("market_mid_at_settle")
    mid_path = settlement.get("mid_path")
    side_edge = _pick(bid, settlement, "side_edge")
    edge = _pick(bid, settlement, "edge")
    runway = _pick(bid, settlement, "runway_seconds")
    if isinstance(runway, (int, float)):
        tags.append(_timing_bucket(float(runway)))
    else:
        tags.append("runway_unknown")

    # Model disagreed with the market favourite on YES mid.
    if isinstance(calibrated_p, (int, float)) and isinstance(market_mid, (int, float)):
        if calibrated_p < market_mid - 0.05:
            tags.append("model_below_yes_mid")  # leans NO / fade YES
        elif calibrated_p > market_mid + 0.05:
            tags.append("model_above_yes_mid")  # leans YES

    if early_exit:
        tags.append("cut_loss_attempted" if close_reason == "CUT_EDGE_REVERSED" else "lock_win_attempted")
    elif won is False:
        tags.append("held_to_settle")
        # Asymmetric payoff: losses near full stake are structural for
        # non-tiny prices.
        if stake > 0 and abs(pnl + stake + fee) < 0.08:
            tags.append("lost_near_full_stake")

    hold_min = None
    bid_ts = bid.get("ts_ms") or settlement.get("bid_ts_ms")
    set_ts = settlement.get("ts_ms")
    if isinstance(bid_ts, (int, float)) and isinstance(set_ts, (int, float)) and set_ts >= bid_ts:
        hold_min = (float(set_ts) - float(bid_ts)) / 60_000.0

    # Primary mode (single headline for UI)
    if early_exit:
        if close_reason == "LOCK_WIN":
            primary = "early_exit_lock_win"
        elif close_reason == "CUT_EDGE_REVERSED":
            primary = "early_exit_cut"
        else:
            primary = "early_exit"
    elif won:
        primary = "win_" + _payoff_bucket(side, price, True).split("_")[0]
    elif price >= 0.65:
        primary = "favourite_lost_full_stake"
    elif price <= 0.35 and side == "NO" and result == "YES":
        primary = "cheap_no_crushed_by_yes"  # faded a high YES that still paid
    elif price <= 0.35 and side == "YES" and result == "NO":
        primary = "cheap_yes_crushed_by_no"
    elif quote == "cross":
        primary = "wrong_side_after_crossing_spread"
    else:
        primary = "wrong_side_held_to_expiry"

    gamma = (
        "cashout_before_settle"
        if early_exit
        else _gamma_factor(
            market_mid, market_mid_at_settle, runway, bool(won), mid_path=mid_path
        )
    )
    tags.append(f"gamma_{gamma}")

    lessons_for_row: List[str] = []
    if early_exit:
        lessons_for_row.append(
            f"paper cashout ({close_reason or 'early_exit'}): sold at "
            f"{exit_price} instead of holding to settlement"
        )
        if pnl < 0:
            lessons_for_row.append(
                "cashout cut a loss (or locked a smaller loss than full stake) — "
                "compare to hold-to-settle counterfactual when path data exists"
            )
        elif pnl > 0:
            lessons_for_row.append(
                "cashout secured a partial/full win before settlement risk"
            )
    elif won is False:
        lessons_for_row.append(HELD_TO_SETTLE)
        if "favourite_lost_full_stake" in primary or price >= 0.65:
            lessons_for_row.append(
                "favourite arithmetic: win pays (1-price), loss pays ~price; "
                "high win-rate can still lose money"
            )
        if "cheap_no_crushed_by_yes" in primary or (
            isinstance(calibrated_p, (int, float))
            and isinstance(market_mid, (int, float))
            and calibrated_p < market_mid
            and side == "NO"
            and result == "YES"
        ):
            lessons_for_row.append(
                "faded a high YES mid that still resolved YES — model edge vs mid "
                "was real in sign but wrong in outcome; do not size these as if "
                "the mid were irrational without path confirmation"
            )
        if quote == "cross":
            lessons_for_row.append(
                "crossed the spread (paid ask); resting would have needed the "
                "market to come to the quote — crossing buys certainty of fill, "
                "not certainty of edge"
            )
        if gamma in ("possible_near_close_noise", "possible_near_close_flip", "path_late_move"):
            lessons_for_row.append(
                "late mid move / near-close entry — last-minute gamma / BRTI "
                "noise can dominate; tighten near-close bans or require confirm"
            )
        if gamma == "mid_stable_wrong_thesis":
            lessons_for_row.append(
                "YES mid at settle ≈ mid at bid — not a late flip; the thesis "
                "was wrong against a market that did not reverse"
            )
        if gamma == "mid_moved_before_settle":
            lessons_for_row.append(
                "YES mid moved between bid and settle — path sampling would "
                "clarify whether a post-fill exit could have helped"
            )

    return {
        "event": "kalshi_bot_bid_postmortem",
        "schema": 1,
        "position_id": settlement.get("position_id"),
        "ticker": ticker,
        "family": _family(ticker),
        "side": side,
        "market_result": result,
        "won": won,
        "early_exit": early_exit,
        "close_reason": close_reason or None,
        "exit_price": exit_price,
        "contracts": contracts,
        "price": price,
        "stake_usd": stake,
        "fee_usd": fee,
        "realized_pnl_usd": pnl,
        "return_on_stake": (pnl / stake) if stake else None,
        "settled_ts": settlement.get("ts"),
        "settled_ts_ms": set_ts,
        "bid_ts": bid.get("ts"),
        "bid_ts_ms": bid_ts,
        "hold_minutes": hold_min,
        "quote_style": quote,
        "quote_style_reason": _pick(bid, settlement, "quote_style_reason"),
        "fill_model": _pick(bid, settlement, "fill_model"),
        "edge": edge,
        "side_edge": side_edge,
        "calibrated_p": calibrated_p,
        "market_mid": market_mid,
        "market_mid_at_settle": market_mid_at_settle,
        "net_ev_usd": _pick(bid, settlement, "net_ev_usd"),
        "cross_cost_usd": _pick(bid, settlement, "cross_cost_usd"),
        "runway_seconds": runway,
        "ttl_ms": _pick(bid, settlement, "ttl_ms"),
        "signal_trusted": _pick(bid, settlement, "signal_trusted"),
        "reason_code": _pick(bid, settlement, "reason_code"),
        "primary_mode": primary,
        "gamma_factor": gamma,
        "fade_ban_lifted": bool(bid.get("fade_ban_lifted")),
        "fade_ban_lift_reason": bid.get("fade_ban_lift_reason"),
        "mid_path_samples": len(mid_path) if isinstance(mid_path, list) else 0,
        "tags": tags,
        "cut_loss": {
            "attempted": early_exit,
            "available": True,
            "reason": (
                f"paper cashout via {close_reason}"
                if early_exit
                else "held to settlement (cashout conditions not met or bid missing)"
            ),
        },
        "lessons": lessons_for_row,
        "bid_joined": bid_joined,
        "snapshot_on_settlement": settlement.get("market_mid") is not None
        or settlement.get("calibrated_p") is not None,
    }


def detect_post_gate_ms(records: List[dict]) -> Optional[int]:
    """Earliest ledger ts that shows fade-lift or mid_path (new-rule markers)."""
    markers: List[int] = []
    for row in records:
        ts = row.get("ts_ms")
        if not isinstance(ts, (int, float)):
            continue
        if row.get("fade_ban_lifted") or row.get("fade_ban_lift_reason"):
            markers.append(int(ts))
            continue
        path = row.get("mid_path")
        if isinstance(path, list) and path:
            markers.append(int(ts))
    return min(markers) if markers else None


def build_postmortems(
    records: List[dict],
    since_ms: Optional[int] = None,
) -> List[dict]:
    bids: Dict[str, dict] = {}
    settlements: List[dict] = []
    for row in records:
        ev = row.get("event")
        if ev == "kalshi_bot_paper_settlement":
            settlements.append(row)
        elif ev == "kalshi_bot_decision" and row.get("action") == "bid":
            pid = row.get("position_id")
            if pid:
                bids[pid] = row

    if since_ms is not None:
        settlements = [
            s for s in settlements
            if isinstance(s.get("ts_ms"), (int, float)) and float(s["ts_ms"]) >= since_ms
        ]
    settlements.sort(key=lambda r: r.get("ts_ms") or 0)
    out = [_classify(s, bids.get(s.get("position_id"))) for s in settlements]
    return out


def summarize(
    rows: List[dict],
    files: List[str],
    gate_filter: Optional[Dict[str, Any]] = None,
    cohort: str = "historic",
) -> dict:
    wins = [r for r in rows if r.get("won") is True]
    losses = [r for r in rows if r.get("won") is False]
    early = [r for r in rows if r.get("early_exit")]
    net = sum(float(r.get("realized_pnl_usd") or 0.0) for r in rows)
    fees = sum(float(r.get("fee_usd") or 0.0) for r in rows)

    mode_counts = collections.Counter(
        r.get("primary_mode") for r in losses + early if r.get("won") is False or r.get("early_exit")
    )
    tag_counts = collections.Counter(t for r in losses for t in r.get("tags") or [])
    family_pnl: Dict[str, float] = collections.defaultdict(float)
    family_wl: Dict[str, List[int]] = collections.defaultdict(lambda: [0, 0])
    for r in rows:
        fam = r.get("family") or "unknown"
        family_pnl[fam] += float(r.get("realized_pnl_usd") or 0.0)
        # early_exit has won=null — count cashouts in early_exits, not W/L
        if r.get("won") is True:
            family_wl[fam][0] += 1
        elif r.get("won") is False:
            family_wl[fam][1] += 1

    worst = sorted(losses, key=lambda r: float(r.get("realized_pnl_usd") or 0.0))[:12]
    best = sorted(wins, key=lambda r: -float(r.get("realized_pnl_usd") or 0.0))[:8]

    def _slim(r: dict) -> dict:
        return {
            "ticker": r.get("ticker"),
            "family": r.get("family"),
            "side": r.get("side"),
            "result": r.get("market_result"),
            "price": r.get("price"),
            "pnl": r.get("realized_pnl_usd"),
            "hold_min": r.get("hold_minutes"),
            "runway_s": r.get("runway_seconds"),
            "side_edge": r.get("side_edge"),
            "calibrated_p": r.get("calibrated_p"),
            "market_mid": r.get("market_mid"),
            "quote": r.get("quote_style"),
            "mode": r.get("primary_mode"),
            "gamma": r.get("gamma_factor"),
            "lessons": r.get("lessons"),
        }

    recommendations = [
        {
            "id": "paper_cashout",
            "priority": 1,
            "claim": (
                f"{len(early)} early_exit cashouts in this record "
                f"(LOCK_WIN / CUT_EDGE_REVERSED at held-side bid); "
                f"{len(losses)} hold-to-settle losses remain"
            ),
            "action": (
                "Paper cashout is ON by default: sell-to-close at floor(held-side bid) "
                "when LOCK_WIN or CUT_EDGE_REVERSED fires; otherwise hold to settlement. "
                "Missing bid fails closed (no mid proxy). Use --no-paper-cashout to disable."
            ),
        },
        {
            "id": "favourite_asymmetry",
            "priority": 2,
            "claim": (
                f"avg win ${ (sum(r['realized_pnl_usd'] for r in wins)/len(wins)) if wins else 0:.2f} "
                f"vs avg loss ${ (sum(r['realized_pnl_usd'] for r in losses)/len(losses)) if losses else 0:.2f} "
                f"on {len(wins)}W/{len(losses)}L"
            ),
            "action": (
                "Prefer sides where downside is capped relative to edge, or "
                "require larger side_edge / lower entry price before crossing. "
                "A ~50% win rate on favourites is not a profit strategy."
            ),
        },
        {
            "id": "fade_yes_near_close",
            "priority": 3,
            "claim": (
                "Many large losses are NO crosses while YES mid was already "
                "high and runway was minutes — model below mid, market still YES."
            ),
            "action": (
                "Ban NO fades of YES mids ≥0.85 with runway ≤10m unless venue "
                "lead confirms down or BRTI avg60 is below open (p_brti < 0.5)."
            ),
        },
        {
            "id": "gamma_honesty",
            "priority": 4,
            "claim": (
                "Last-second 'gamma' cannot be blamed without a mid path; "
                "settlements now carry mid_path samples when the paper tick "
                "observed them."
            ),
            "action": (
                "Use settlement mid_path (≥2 samples) for path_late_move vs "
                "stable-wrong; fall back to mid_at_settle / runway only when "
                "path is absent."
            ),
        },
    ]

    return {
        "event": "kalshi_bot_postmortem_summary",
        "schema": 2,
        "cohort": cohort,
        "files": files,
        "settled": len(rows),
        "wins": len(wins),
        "losses": len(losses),
        "early_exits": len(early),
        "net_realized_pnl_usd": round(net, 2),
        "fees_usd": round(fees, 2),
        "win_rate": (len(wins) / (len(wins) + len(losses))) if (wins or losses) else None,
        "mean_win_usd": (sum(r["realized_pnl_usd"] for r in wins) / len(wins)) if wins else None,
        "mean_loss_usd": (sum(r["realized_pnl_usd"] for r in losses) / len(losses)) if losses else None,
        "loss_primary_modes": mode_counts.most_common(),
        "loss_tags": tag_counts.most_common(20),
        "by_family": {
            fam: {
                "wins": family_wl[fam][0],
                "losses": family_wl[fam][1],
                "early_exits": sum(
                    1 for r in rows if (r.get("family") or "unknown") == fam and r.get("early_exit")
                ),
                "net_pnl_usd": round(family_pnl[fam], 2),
            }
            for fam in sorted(family_pnl)
        },
        "worst_losses": [_slim(r) for r in worst],
        "best_wins": [_slim(r) for r in best],
        "cut_loss_policy": {
            "post_fill_exit_exists": True,
            "reason": (
                "paper cashout: LOCK_WIN / CUT_EDGE_REVERSED sell at held-side bid; "
                "hold-to-settle when conditions unmet or bid missing"
            ),
        },
        "recommendations": recommendations,
        "joined_bid_rate": (
            sum(1 for r in rows if r.get("bid_joined")) / len(rows) if rows else None
        ),
        "measurement": {
            "gate_filter": gate_filter,
            "fade_ban_lifts": sum(1 for r in rows if r.get("fade_ban_lifted")),
            "early_exits": len(early),
            "cheap_no_crushed_by_yes": mode_counts.get("cheap_no_crushed_by_yes", 0),
            "favourite_lost_full_stake": mode_counts.get("favourite_lost_full_stake", 0),
            "wrong_side_after_crossing_spread": mode_counts.get(
                "wrong_side_after_crossing_spread", 0
            ),
            "mean_loss_usd": (
                (sum(r["realized_pnl_usd"] for r in losses) / len(losses)) if losses else None
            ),
            "settlements_with_mid_path": sum(
                1 for r in rows if int(r.get("mid_path_samples") or 0) >= 2
            ),
        },
    }


def write_artifacts(
    rows: List[dict],
    summary: dict,
    jsonl_name: str = POSTMORTEM_JSONL,
    summary_name: str = SUMMARY_JSON,
) -> None:
    root = _evidence()
    jsonl_path = root / jsonl_name
    with jsonl_path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    summary_path = root / summary_name
    with summary_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")


def _print_human(summary: dict, title: Optional[str] = None) -> None:
    cohort = summary.get("cohort") or "historic"
    print(title or f"KALSHI BOT BID POSTMORTEM · {cohort.upper()}")
    meas = summary.get("measurement") or {}
    gf = meas.get("gate_filter") or {}
    if gf:
        print(
            f"filter: {gf.get('mode')} since_ms={gf.get('since_ms')} "
            f"({gf.get('detail') or ''})".rstrip()
        )
    early = summary.get("early_exits") or 0
    print(
        f"settled {summary.get('settled')} · "
        f"{summary.get('wins')}W / {summary.get('losses')}L · "
        f"{early} cashouts · "
        f"net ${summary.get('net_realized_pnl_usd')} · "
        f"fees ${summary.get('fees_usd')}"
    )
    mw = summary.get("mean_win_usd")
    ml = summary.get("mean_loss_usd")
    wr = summary.get("win_rate")
    print(
        f"mean win ${mw:.2f}" if isinstance(mw, (int, float)) else "mean win n/a",
        end="",
    )
    print(
        f" · mean loss ${ml:.2f}" if isinstance(ml, (int, float)) else " · mean loss n/a",
        end="",
    )
    print(f" · win rate {wr:.1%}" if isinstance(wr, (int, float)) else "")
    print(f"cut-loss: {summary.get('cut_loss_policy', {}).get('reason')}")
    if meas:
        print(
            "measurement: "
            f"fade_lifts={meas.get('fade_ban_lifts')}  "
            f"early_exits={meas.get('early_exits')}  "
            f"cheap_no={meas.get('cheap_no_crushed_by_yes')}  "
            f"fav_lost={meas.get('favourite_lost_full_stake')}  "
            f"cross_wrong={meas.get('wrong_side_after_crossing_spread')}  "
            f"mid_path≥2={meas.get('settlements_with_mid_path')}"
        )
    print("by family:")
    for fam, row in (summary.get("by_family") or {}).items():
        early_n = row.get("early_exits") or 0
        print(
            f"  {fam}: {row['wins']}W/{row['losses']}L"
            f"{f' · {early_n} cashouts' if early_n else ''}"
            f" net ${row['net_pnl_usd']}"
        )
    print("loss modes:")
    for mode, n in summary.get("loss_primary_modes") or []:
        print(f"  {n:3d}  {mode}")
    print("worst losses:")
    for row in summary.get("worst_losses") or []:
        print(
            f"  {row.get('pnl'):+.2f}  {row.get('ticker')}  "
            f"{row.get('side')}@{row.get('price')} → {row.get('result')}  "
            f"mode={row.get('mode')}  gamma={row.get('gamma')}  "
            f"runway={row.get('runway_s')}s  quote={row.get('quote')}"
        )
        for lesson in row.get("lessons") or []:
            print(f"      · {lesson}")
    print("recommendations:")
    for rec in summary.get("recommendations") or []:
        print(f"  [{rec.get('priority')}] {rec.get('id')}")
        print(f"      {rec.get('claim')}")
        print(f"      → {rec.get('action')}")


def _current_gate_filter(
    records: List[dict],
    *,
    force_post_gate: bool,
    since_ms: Optional[int],
) -> Tuple[Optional[int], Optional[Dict[str, Any]], Optional[str]]:
    """Resolve current-rules window. Returns (since_ms, filter, error)."""
    detected = detect_post_gate_ms(records)
    if force_post_gate and detected is None and since_ms is None:
        return None, None, (
            "postmortem: --post-gate found no fade_ban_lifted / mid_path "
            "markers yet — accumulate paper under the new rules first"
        )
    if since_ms is not None and not force_post_gate:
        return since_ms, {"mode": "since-ms", "since_ms": since_ms, "detail": ""}, None
    if detected is None and since_ms is None:
        return None, None, None
    window = detected if since_ms is None else (
        max(since_ms, detected) if detected is not None else since_ms
    )
    if force_post_gate or detected is not None:
        detail = "first fade_ban_lifted or mid_path marker"
        if since_ms is not None and detected is not None and since_ms > detected:
            detail = f"{detail}; clamped by --since-ms"
        return window, {
            "mode": "post-gate",
            "since_ms": window,
            "detail": detail,
        }, None
    return window, {"mode": "since-ms", "since_ms": window, "detail": ""}, None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json",
        action="store_true",
        help="print dual-cohort summary JSON on stdout (default: human text)",
    )
    parser.add_argument("--no-write", action="store_true", help="do not write evidence files")
    parser.add_argument(
        "--since-ms",
        type=int,
        default=None,
        help="override current-rules window start (historic still written full)",
    )
    parser.add_argument(
        "--post-gate",
        action="store_true",
        help=(
            "require current-rules window from first fade_ban_lifted / mid_path "
            "marker (still writes full historic); exit 3 if no marker yet"
        ),
    )
    args = parser.parse_args(argv)

    records, files = _load_ledger()

    historic_rows = build_postmortems(records, since_ms=None)
    historic = summarize(historic_rows, files, gate_filter=None, cohort="historic")

    since_ms, current_filter, err = _current_gate_filter(
        records, force_post_gate=args.post_gate, since_ms=args.since_ms
    )
    if err:
        sys.stderr.write(err + "\n")
        return 3

    current = None
    current_rows: List[dict] = []
    if current_filter is not None and since_ms is not None:
        current_rows = build_postmortems(records, since_ms=since_ms)
        current = summarize(
            current_rows, files, gate_filter=current_filter, cohort="current_rules"
        )

    if not args.no_write:
        write_artifacts(historic_rows, historic, POSTMORTEM_JSONL, SUMMARY_JSON)
        sys.stderr.write(
            f"postmortem: wrote historic {POSTMORTEM_JSONL} "
            f"({len(historic_rows)} rows) + {SUMMARY_JSON}\n"
        )
        if current is not None:
            write_artifacts(
                current_rows, current, CURRENT_POSTMORTEM_JSONL, CURRENT_SUMMARY_JSON
            )
            sys.stderr.write(
                f"postmortem: wrote current-rules {CURRENT_POSTMORTEM_JSONL} "
                f"({len(current_rows)} rows) + {CURRENT_SUMMARY_JSON}\n"
            )
        else:
            # Clear stale current artifacts so cockpit does not judge new rules
            # from a previous window after markers disappear.
            root = _evidence()
            for name in (CURRENT_POSTMORTEM_JSONL, CURRENT_SUMMARY_JSON):
                path = root / name
                if path.exists():
                    path.unlink()
            sys.stderr.write(
                "postmortem: no current-rules window yet "
                "(no mid_path / fade_ban_lifted markers) — historic only\n"
            )

    payload = {"historic": historic, "current_rules": current}
    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        if current is not None:
            _print_human(
                current,
                "KALSHI BOT BID POSTMORTEM · CURRENT RULES "
                "(judge new trading here — not lifetime losses)",
            )
            print()
        _print_human(
            historic,
            "KALSHI BOT BID POSTMORTEM · HISTORIC "
            "(learning + sealed-gate ledger — not the new-rules scorecard)",
        )
        if current is None:
            print()
            print(
                "CURRENT RULES: unavailable — accumulate paper with mid_path / "
                "fade_ban_lifted markers, then re-run postmortem"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
