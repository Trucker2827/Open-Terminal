#!/usr/bin/env python3
"""Mechanical, epoch-scoped Claude-vs-Codex shadow competition report."""
import argparse
import json
import math
import os
import random
import sqlite3
import time

CLAUDE_EPOCH = "kalshi-blind-claude-cli-v1"
CODEX_EPOCH = "kalshi-blind-codex-v3-zero-capability"
MIN_PAIRED = 200
MIN_COVERAGE = 0.80


def compute_result_state(paired, claude_coverage, codex_coverage, ci_low, ci_high,
                         invalid=False):
    if invalid:
        return "INVALID_EPOCH"
    if paired < MIN_PAIRED or min(claude_coverage, codex_coverage) < MIN_COVERAGE:
        return "INSUFFICIENT_PAIRED_DATA"
    if ci_low <= 0 <= ci_high:
        return "STATISTICAL_TIE"
    # Delta is Claude Brier minus Codex Brier: lower is better.
    return "CLAUDE_WINS" if ci_high < 0 else "CODEX_WINS"


def bootstrap_ci(deltas, iterations=2000):
    if not deltas:
        return 0.0, 0.0
    rng = random.Random(0xC0DEC1A)
    means = []
    for _ in range(iterations):
        means.append(sum(rng.choice(deltas) for _ in deltas) / len(deltas))
    means.sort()
    return means[int(.025 * (len(means) - 1))], means[int(.975 * (len(means) - 1))]


def load_jsonl(path):
    rows = []
    try:
        with open(path) as handle:
            for line in handle:
                try: rows.append(json.loads(line))
                except json.JSONDecodeError: pass
    except OSError:
        pass
    return rows


def resolved_outcomes(db_path, ids):
    if not ids or not os.path.exists(db_path):
        return {}
    connection = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
    try:
        marks = ",".join("?" for _ in ids)
        return {row[0]: int(row[1]) for row in connection.execute(
            "SELECT id,outcome FROM edge_decision_journal WHERE id IN (" + marks + ") AND outcome IN (0,1)", ids)}
    finally:
        connection.close()


def build_report(rows, outcomes=None, firewall_safe=True):
    outcomes = outcomes or {}
    opportunities = [row for row in rows if row.get("event") == "shadow_opportunity" and row.get("lanes")]
    counts = {"claude": {"predicted": 0, "abstained": 0},
              "codex": {"predicted": 0, "abstained": 0}}
    paired_rows = []
    forecast_rows = []
    invalid_reasons = []
    for row in opportunities:
        lanes = row.get("lanes", [])
        if len(lanes) != 2:
            invalid_reasons.append("PAIRING_CORRUPTION")
            continue
        by_provider = {lane.get("forecaster", {}).get("provider", ""): lane for lane in lanes}
        claude = by_provider.get("anthropic-claude-cli")
        codex = by_provider.get("openai-codex-cli")
        if not claude or not codex:
            invalid_reasons.append("PAIR_IDENTITY_MISMATCH")
            continue
        if claude.get("context_hash") != codex.get("context_hash"):
            invalid_reasons.append("CONTEXT_HASH_DIVERGENCE")
        ch = claude.get("forecast", {}).get("prompt_hash")
        co = codex.get("forecast", {}).get("prompt_hash")
        if ch and co and ch != co:
            invalid_reasons.append("PROMPT_HASH_DIVERGENCE")
        for name, lane in (("claude", claude), ("codex", codex)):
            predicted = lane.get("status") == "COMMITTED_BLIND"
            counts[name]["predicted" if predicted else "abstained"] += 1
        c_id, o_id = claude.get("journal_id"), codex.get("journal_id")
        summary = {"competition_pair_id": row.get("competition_pair_id"), "ticker": row.get("ticker"),
            "context_hash": row.get("context_hash"), "blind_context": row.get("blind_context", {}),
            "opened_at_ms": row.get("opened_at_ms"), "claude_status": claude.get("status"),
            "codex_status": codex.get("status"), "claude_probability": claude.get("forecast", {}).get("probability"),
            "codex_probability": codex.get("forecast", {}).get("probability"),
            "claude_rationale": claude.get("forecast", {}).get("rationale", ""),
            "codex_rationale": codex.get("forecast", {}).get("rationale", ""),
            "claude_reason_code": claude.get("reason_code", claude.get("forecast", {}).get("reason_code", "")),
            "codex_reason_code": codex.get("reason_code", codex.get("forecast", {}).get("reason_code", "")),
            "claude_prompt_hash": claude.get("forecast", {}).get("prompt_hash", ""),
            "codex_prompt_hash": codex.get("forecast", {}).get("prompt_hash", ""),
            "claude_sealed_hash": claude.get("sealed_hash", ""), "codex_sealed_hash": codex.get("sealed_hash", ""),
            "market_at_open": claude.get("market_at_open", codex.get("market_at_open", {}))}
        if c_id in outcomes and o_id in outcomes and outcomes[c_id] == outcomes[o_id]:
            c_p = float(claude["forecast"]["probability"])
            o_p = float(codex["forecast"]["probability"])
            y = outcomes[c_id]
            paired_rows.append({"competition_pair_id": row.get("competition_pair_id"),
                "ticker": row.get("ticker"), "outcome": y, "claude_probability": c_p,
                "codex_probability": o_p, "claude_brier": (c_p-y)**2,
                "codex_brier": (o_p-y)**2, "context_hash": row.get("context_hash"),
                "claude_rationale": claude["forecast"].get("rationale", ""),
                "codex_rationale": codex["forecast"].get("rationale", "")})
            summary.update(outcome=y, claude_brier=(c_p-y)**2, codex_brier=(o_p-y)**2,
                           forecast_winner="CLAUDE" if (c_p-y)**2 < (o_p-y)**2 else
                           "CODEX" if (o_p-y)**2 < (c_p-y)**2 else "TIE")
        forecast_rows.append(summary)
    total = len(opportunities)
    coverage = {name: (values["predicted"] / total if total else 0.0) for name, values in counts.items()}
    deltas = [row["claude_brier"] - row["codex_brier"] for row in paired_rows]
    low, high = bootstrap_ci(deltas)
    invalid = bool(invalid_reasons) or not firewall_safe
    state = compute_result_state(len(paired_rows), coverage["claude"], coverage["codex"], low, high, invalid)
    return {"schema_version": "kalshi-competition-report-v1", "generated_at_ms": int(time.time()*1000),
        "result_state": state, "shadow_only": True, "execution_eligible": False,
        "epoch_pair": {"claude": CLAUDE_EPOCH, "codex": CODEX_EPOCH},
        "thresholds": {"minimum_jointly_resolved": MIN_PAIRED, "minimum_coverage_each": MIN_COVERAGE},
        "opportunities": total, "jointly_resolved": len(paired_rows), "coverage": coverage,
        "rates": counts, "paired_brier_delta_claude_minus_codex": sum(deltas)/len(deltas) if deltas else 0.0,
        "ci_low": low, "ci_high": high, "invalid_reasons": sorted(set(invalid_reasons)),
        "firewall_safe": firewall_safe, "paired_forecasts": paired_rows[-250:],
        "forecasts": forecast_rows[-250:]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--journal", required=True)
    parser.add_argument("--db", required=True)
    parser.add_argument("--firewall")
    parser.add_argument("--output")
    args = parser.parse_args()
    rows = load_jsonl(args.journal)
    ids = [lane.get("journal_id") for row in rows for lane in row.get("lanes", []) if lane.get("journal_id")]
    safe = True
    if args.firewall:
        try: safe = bool(json.load(open(args.firewall)).get("safe"))
        except (OSError, json.JSONDecodeError): safe = False
    report = build_report(rows, resolved_outcomes(args.db, ids), safe)
    payload = json.dumps(report, sort_keys=True, indent=2)
    if args.output:
        temporary = args.output + ".tmp"
        with open(temporary, "w") as handle: handle.write(payload + "\n")
        os.replace(temporary, args.output)
    print(payload)


if __name__ == "__main__":
    main()
