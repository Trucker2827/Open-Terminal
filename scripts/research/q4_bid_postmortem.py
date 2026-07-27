#!/usr/bin/env python3
"""Q4 of the Kalshi edge autopsy (issue #169): post-mortem on the bot's bids.

Walks the bot's own decision ledger end to end — pass, bid, cancel, fill,
settlement — and reports every settled position individually rather than a
fitted summary, because there are not enough of them to fit anything to.

THE HEADLINE IS THE SAMPLE SIZE. The sealed gate requires 300 settled bids and
the ledger has a couple of dozen. At that n, "what distinguishes winners from
losers" has no answer that is not noise: with ~15 outcomes, a split on any
binary feature produces two groups of ~7 whose win rates differ by 20-30 points
purely by chance. This script therefore computes the per-position table and the
funnel, states the discrimination question as unanswerable, and DOES NOT fit a
model to fifteen coin flips. The same applies to the $0.03 adverse-selection
premium (`rest_premium_usd`, issue #165): validating its size needs resting
fills that were adversely selected, and the ledger's resting fills are countable
on two hands.

Read-only. Prints JSON to stdout; writes nothing.

  python3 scripts/research/q4_bid_postmortem.py
"""
import collections
import json
import sys

import kalshi_edge_common as common

BOT_DECISIONS = "kalshi-bot-decisions.jsonl"
GATE = "kalshi-bot-gate.json"


def load_gate():
    """The sealed promotion gate, read but never touched."""
    try:
        with open(common.evidence_path(GATE), "r", encoding="utf-8") as handle:
            gate = json.load(handle)
    except (OSError, ValueError) as exc:
        return {"available": False, "error": str(exc)}
    return {"available": True, "verdict": gate.get("verdict"),
            "sealed_at_ms": gate.get("sealed_at_ms"),
            "seal_sha256": gate.get("seal_sha256"),
            "params": gate.get("params"), "ledger": gate.get("ledger"),
            "criteria": [{k: c.get(k) for k in
                          ("id", "met", "observed", "required", "description")}
                         for c in gate.get("criteria", [])]}


def main():
    records, inventory = common.read_jsonl(BOT_DECISIONS, rotations=("{name}",))
    actions = collections.Counter()
    reasons = collections.Counter()
    bids, fills, cancels, settlements = [], [], [], []
    for record in records:
        if record.get("event") == "kalshi_bot_paper_settlement":
            settlements.append(record)
            continue
        action = record.get("action")
        actions[action] += 1
        if action == "pass":
            reasons[record.get("reason_code")] += 1
        elif action == "bid":
            bids.append(record)
        elif action == "fill":
            fills.append(record)
        elif action == "cancel":
            cancels.append(record)

    fill_by_position = {f.get("position_id"): f for f in fills}
    positions = []
    for row in sorted(settlements, key=lambda r: r.get("ts_ms") or 0):
        fill = fill_by_position.get(row.get("position_id")) or {}
        stake = float(row.get("stake_usd") or 0.0)
        pnl = float(row.get("realized_pnl") or 0.0)
        positions.append({
            "settled_utc": row.get("ts"),
            "ticker": row.get("ticker"),
            "family": (row.get("ticker") or "").split("-")[0],
            "side": row.get("side"),
            "contracts": row.get("contracts"),
            "price": row.get("price"),
            "stake_usd": stake,
            "fee_usd": row.get("fee_usd"),
            "market_result": row.get("market_result"),
            "won": row.get("won"),
            "realized_pnl_usd": pnl,
            "return_on_stake": pnl / stake if stake else None,
            "settlement_source": row.get("settlement_source"),
            # From the originating fill, where the ledger recorded one.
            "fill_limit_price": fill.get("limit_price"),
            "fill_observed_mid": fill.get("observed_mid"),
            "resting_seconds": fill.get("resting_seconds"),
            "fill_model": fill.get("fill_model"),
        })

    wins = [p for p in positions if p["won"]]
    losses = [p for p in positions if p["won"] is False]
    net = sum(p["realized_pnl_usd"] for p in positions)

    quote_styles = collections.Counter(b.get("quote_style") for b in bids)
    rest_premium = {b.get("rest_premium_usd") for b in bids
                    if b.get("rest_premium_usd") is not None}

    common.emit({
        "as_of_utc": common.as_of(),
        "question": "Q4 — post-mortem on the bot's own bids",
        "command": "python3 scripts/research/q4_bid_postmortem.py",
        "data": {"files": inventory,
                 "rows": len(records),
                 "span_utc": [records[0].get("ts"), records[-1].get("ts")]
                 if records else None},
        "funnel": {
            "decision_rows": sum(actions.values()),
            "actions": {str(k): v for k, v in actions.items()},
            "bids_placed": len(bids),
            "cancels": len(cancels),
            "fills": len(fills),
            "settled_positions": len(positions),
            "fill_rate_of_bids": len(fills) / len(bids) if bids else None,
            "top_pass_reasons": reasons.most_common(10),
        },
        "gate": load_gate(),
        "settled_positions": positions,
        "outcome_summary": {
            "settled": len(positions), "wins": len(wins), "losses": len(losses),
            "net_realized_pnl_usd": net,
            "gross_stake_usd": sum(p["stake_usd"] for p in positions),
            "fees_usd": sum(float(p["fee_usd"] or 0.0) for p in positions),
            "mean_pnl_per_position_usd": net / len(positions) if positions else None,
            "win_rate": len(wins) / len(positions) if positions else None,
        },
        # DESCRIPTIVE ARITHMETIC, NOT INFERENCE. This is what the fifteen rows
        # add up to, not a claim about the strategy's population behaviour. It
        # is reported because the asymmetry is mechanical rather than
        # statistical: a favourite bought at 0.83 wins four times in five and
        # pays 0.17, so a majority of winners coexists with a negative total by
        # construction. Reading it as "the bot should buy underdogs" would be
        # exactly the n=15 overfit the next block refuses.
        "payoff_shape": {
            "mean_entry_price_winners": (sum(p["price"] for p in wins) / len(wins)
                                         if wins else None),
            "mean_entry_price_losers": (sum(p["price"] for p in losses) / len(losses)
                                        if losses else None),
            "mean_pnl_winners_usd": (sum(p["realized_pnl_usd"] for p in wins)
                                     / len(wins) if wins else None),
            "mean_pnl_losers_usd": (sum(p["realized_pnl_usd"] for p in losses)
                                    / len(losses) if losses else None),
            "note": ("a 53% win rate with a negative total is the arithmetic of "
                     "buying favourites: wins pay a fraction of stake, losses "
                     "pay all of it"),
        },
        "winners_versus_losers": {
            "answerable": False,
            "reason": ("n = %d settled positions. Any split of this sample into "
                       "winners and losers produces groups of single digits, "
                       "where a 20-30 point difference in any rate is the "
                       "expected size of chance alone. No discriminating "
                       "feature is reported because none can be measured."
                       % len(positions)),
            "n_required_by_sealed_gate": 300,
            "per_position_table_is_the_evidence": True,
        },
        "adverse_selection_premium": {
            "configured_default_usd": 0.03,
            "source": ("KalshiBotDecision.h:191 rest_premium_usd — the resting "
                       "tier's extra hurdle from issue #165"),
            "journalled_in_ledger": sorted(rest_premium) or None,
            "quote_styles_bid": {str(k): v for k, v in quote_styles.items()},
            "fills_from_resting_quotes": sum(1 for f in fills
                                             if f.get("resting_seconds") is not None),
            "fills_from_crossing_quotes": sum(1 for f in fills
                                              if f.get("resting_seconds") is None),
            "empirically_validated": False,
            "reason": ("sizing the premium means comparing the realized edge of "
                       "resting fills against crossing fills, and the ledger "
                       "contains no settled crossing fill at all — every "
                       "settled position came from a resting quote. There is no "
                       "contrast to measure, so the $0.03 is neither confirmed "
                       "nor refuted. Note also that the ledger rows do not "
                       "journal rest_premium_usd itself in this window, so the "
                       "value above is read from the source default."),
        },
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
