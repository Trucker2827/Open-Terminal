#!/usr/bin/env python3
"""Pure/stateful primitives shared by shadow and future canary execution."""
from __future__ import annotations

import hashlib
import json
import math
import os
import tempfile
import time
from dataclasses import asdict, dataclass
from typing import Any

PROPOSAL_SCHEMA_VERSION = "kalshi-order-proposal-v1"
GATE_POLICY_VERSION = "kalshi-shadow-gate-v1"
QUALIFICATION_POLICY_VERSION = "kalshi-qualification-v1"
SAFETY_POLICY_VERSION = "kalshi-canary-safety-v1"
PROMOTION_POLICY_VERSION = "kalshi-promotion-v1"


@dataclass(frozen=True)
class GatePolicy:
    minimum_confidence: float = 0.55
    minimum_net_edge: float = 0.03
    maximum_spread: float = 0.05
    minimum_depth: float = 1.0
    maximum_order_contracts: int = 1
    proposal_ttl_ms: int = 5_000


@dataclass(frozen=True)
class QualificationPolicy:
    minimum_resolved: int = 200
    minimum_daemon_coverage: float = 0.80
    minimum_net_value: float = 0.0
    require_positive_ci_low: bool = True

@dataclass(frozen=True)
class SafetyPolicy:
    maximum_daily_loss: float = 5.0
    maximum_drawdown: float = 10.0
    maximum_consecutive_losses: int = 3
    maximum_open_exposure: float = 5.0
    maximum_reconciliation_age_ms: int = 30_000

def default_safety_state(now_ms: int = 0) -> dict:
    return {"policy_version":SAFETY_POLICY_VERSION,"day":"","daily_realized_pnl":0.0,
            "equity_peak":0.0,"equity_current":0.0,"maximum_drawdown":0.0,
            "consecutive_losses":0,"open_exposure":0.0,"submission_unknown_count":0,
            "pnl_reconciliation_pending":0,
            "last_reconciled_at_ms":now_ms,"paused":False,"pause_reason":"","updated_at_ms":now_ms}

def evaluate_safety(state: dict, now_ms: int, policy: SafetyPolicy = SafetyPolicy()) -> dict:
    blockers=[]
    if state.get("paused"): blockers.append("paused:"+str(state.get("pause_reason","unspecified")))
    if float(state.get("daily_realized_pnl",0)) <= -policy.maximum_daily_loss: blockers.append("daily_loss_limit")
    if float(state.get("maximum_drawdown",0)) >= policy.maximum_drawdown: blockers.append("drawdown_limit")
    if int(state.get("consecutive_losses",0)) >= policy.maximum_consecutive_losses: blockers.append("consecutive_loss_limit")
    if float(state.get("open_exposure",0)) >= policy.maximum_open_exposure: blockers.append("exposure_limit")
    if int(state.get("submission_unknown_count",0)) > 0: blockers.append("submission_unknown")
    if int(state.get("pnl_reconciliation_pending",0)) > 0: blockers.append("pnl_reconciliation_pending")
    reconciled=int(state.get("last_reconciled_at_ms",0))
    if reconciled <= 0 or now_ms-reconciled > policy.maximum_reconciliation_age_ms: blockers.append("reconciliation_stale")
    return {"policy_version":SAFETY_POLICY_VERSION,"safe":not blockers,"blockers":blockers,"policy":asdict(policy)}

def record_safety_observation(state: dict, *, now_ms: int, realized_pnl: float = 0.0,
                              equity: float | None = None, open_exposure: float | None = None,
                              submission_unknown_count: int | None = None, reconciled: bool = False) -> dict:
    out=dict(state or default_safety_state()); day=time.strftime("%Y-%m-%d",time.gmtime(now_ms/1000))
    if out.get("day") != day: out.update(day=day,daily_realized_pnl=0.0,consecutive_losses=0)
    out["daily_realized_pnl"]=float(out.get("daily_realized_pnl",0))+realized_pnl
    out["consecutive_losses"]=(int(out.get("consecutive_losses",0))+1 if realized_pnl < 0
                                else 0 if realized_pnl > 0 else int(out.get("consecutive_losses",0)))
    if equity is not None:
        out["equity_current"]=equity;out["equity_peak"]=max(float(out.get("equity_peak",equity)),equity)
        out["maximum_drawdown"]=max(float(out.get("maximum_drawdown",0)),out["equity_peak"]-equity)
    if open_exposure is not None:out["open_exposure"]=max(0.0,open_exposure)
    if submission_unknown_count is not None:out["submission_unknown_count"]=max(0,submission_unknown_count)
    if reconciled:out["last_reconciled_at_ms"]=now_ms
    out["updated_at_ms"]=now_ms;return out

def promotion_transition(current: dict, qual: dict, safety: dict, action: str, now_ms: int) -> dict:
    state=str(current.get("state","SHADOW")); reason=""
    safe=bool(safety.get("safe"))
    if action=="enable_canary" and not safe:
        raise ValueError("canary requires all safety gates clear")
    if action=="enable_canary" and (state!="QUALIFIED" or not qual.get("qualified")):
        raise ValueError("canary requires current qualified state")
    if not safe: state="PAUSED";reason="safety:"+",".join(safety.get("blockers",[]))
    elif action=="evaluate":
        state="QUALIFIED" if qual.get("qualified") else ("DEMOTED" if state in ("QUALIFIED","CANARY_ENABLED") else "SHADOW")
        reason="qualification_passed" if qual.get("qualified") else "qualification_failed"
    elif action=="enable_canary":
        state="CANARY_ENABLED";reason="explicit_enable_after_qualification"
    elif action=="disable_canary": state="QUALIFIED" if qual.get("qualified") else "SHADOW";reason="explicit_disable"
    elif action=="pause": state="PAUSED";reason="explicit_pause"
    elif action=="resume": state="QUALIFIED" if qual.get("qualified") else "SHADOW";reason="explicit_resume"
    else: raise ValueError("unknown promotion action")
    return {"policy_version":PROMOTION_POLICY_VERSION,"state":state,"reason":reason,
            "qualification_policy_version":qual.get("policy_version"),"safety_policy_version":safety.get("policy_version"),
            "updated_at_ms":now_ms}

def comparative_proposals(challenge_id, ticker, codex_probability, daemon_probability,
                          confidence, market, now_ms, relevance_at, policy=GatePolicy()):
    rows={"advisor":build_order_proposal(challenge_id,ticker,codex_probability,confidence,market,now_ms,relevance_at,policy)}
    if daemon_probability is not None and 0 <= daemon_probability <= 1:
        rows["daemon"]=build_order_proposal(challenge_id,ticker,daemon_probability,1.0,market,now_ms,relevance_at,policy)
        consensus=(codex_probability+daemon_probability)/2
        rows["consensus"]=build_order_proposal(challenge_id,ticker,consensus,min(confidence,1.0),market,now_ms,relevance_at,policy)
        rows["disagreement"]={"absolute_probability":abs(codex_probability-daemon_probability),
                              "same_side":rows["advisor"]["side"]==rows["daemon"]["side"]}
    return rows


def canonical(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def validate_forecast(raw: dict) -> dict:
    decision = str(raw.get("decision", "")).strip().lower()
    if decision == "abstain":
        reason = str(raw.get("reason_code", "")).strip().upper()
        if not reason:
            raise ValueError("abstention requires reason_code")
        if "probability" in raw:
            raise ValueError("abstention must not include probability")
        confidence = float(raw.get("confidence", 0.0))
        if not 0.0 <= confidence <= 1.0:
            raise ValueError("confidence must be 0..1")
        return {"decision": "abstain", "reason_code": reason,
                "confidence": confidence, "rationale": str(raw.get("rationale", ""))[:500]}
    if decision != "predict":
        raise ValueError("decision must be predict or abstain")
    probability = float(raw["probability"])
    confidence = float(raw["confidence"])
    if not 0.0 <= probability <= 1.0 or not 0.0 <= confidence <= 1.0:
        raise ValueError("probability and confidence must be 0..1")
    return {"decision": "predict", "probability": probability, "confidence": confidence,
            "rationale": str(raw.get("rationale", ""))[:500]}


def fee_per_contract(price: float) -> float:
    p = min(1.0, max(0.0, price))
    return math.ceil(0.07 * p * (1.0 - p) * 100.0) / 100.0


def build_order_proposal(challenge_id: str, ticker: str, probability_yes: float,
                         confidence: float, market: dict, now_ms: int,
                         execution_relevance_at: int, policy: GatePolicy) -> dict:
    yes_bid, yes_ask = float(market.get("yes_bid", 0)), float(market.get("yes_ask", 0))
    no_bid, no_ask = float(market.get("no_bid", 0)), float(market.get("no_ask", 0))
    side = "yes" if probability_yes >= float(market.get("market_implied_probability", .5)) else "no"
    bid, ask = (yes_bid, yes_ask) if side == "yes" else (no_bid, no_ask)
    depth = float(market.get("yes_depth" if side == "yes" else "no_depth", 0))
    side_probability = probability_yes if side == "yes" else 1.0 - probability_yes
    fee = fee_per_contract(ask)
    spread = ask - bid if ask > 0 and bid > 0 else 1.0
    net_edge = side_probability - ask - fee
    blockers = []
    if now_ms >= execution_relevance_at: blockers.append("execution_relevance_expired")
    if confidence < policy.minimum_confidence: blockers.append("confidence_below_minimum")
    if ask <= 0 or ask >= 1: blockers.append("non_executable_ask")
    if spread > policy.maximum_spread: blockers.append("spread_above_maximum")
    if depth < policy.minimum_depth: blockers.append("depth_below_minimum")
    if net_edge < policy.minimum_net_edge: blockers.append("cost_net_edge_below_minimum")
    return {
        "schema_version": PROPOSAL_SCHEMA_VERSION, "gate_policy_version": GATE_POLICY_VERSION,
        "authority": "advisory_only", "execution_mode": "shadow", "execution_eligible": False,
        "challenge_id": challenge_id, "ticker": ticker, "side": side,
        "order_type": "limit", "limit_price": ask,
        "quantity": min(policy.maximum_order_contracts, max(0, int(depth))),
        "ttl_ms": policy.proposal_ttl_ms, "created_at_ms": now_ms,
        "expires_at_ms": min(now_ms + policy.proposal_ttl_ms, execution_relevance_at),
        "probability_side": side_probability, "confidence": confidence,
        "bid": bid, "ask": ask, "depth": depth, "spread": spread,
        "fee_per_contract": fee, "cost_net_edge": net_edge,
        "gate": "pass" if not blockers else "reject", "blockers": blockers,
    }


class ImmutableJournal:
    def __init__(self, path: str): self.path = path

    def append(self, event: dict) -> dict:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        previous = "0" * 64
        if os.path.exists(self.path):
            with open(self.path, "rb") as f:
                for line in f:
                    if line.strip(): previous = json.loads(line)["event_hash"]
        row = dict(event)
        row["previous_hash"] = previous
        row["event_hash"] = hashlib.sha256(previous.encode() + canonical(row)).hexdigest()
        with open(self.path, "a", encoding="utf-8") as f:
            f.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
            f.flush(); os.fsync(f.fileno())
        return row

    def read(self) -> list[dict]:
        if not os.path.exists(self.path): return []
        with open(self.path, encoding="utf-8") as f: return [json.loads(x) for x in f if x.strip()]

    def verify(self) -> bool:
        previous = "0" * 64
        for stored in self.read():
            row = dict(stored); actual = row.pop("event_hash", "")
            if row.get("previous_hash") != previous: return False
            if hashlib.sha256(previous.encode() + canonical(row)).hexdigest() != actual: return False
            previous = actual
        return True


def write_state(path: str, state: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix="advisor-state-", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(state, f, sort_keys=True); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)


def qualification(score: dict, policy: QualificationPolicy = QualificationPolicy()) -> dict:
    resolved = int(score.get("n_resolved", 0))
    comparable = int(score.get("daemon_comparable", 0))
    coverage = comparable / resolved if resolved else 0.0
    net = float((score.get("net_value_after_fees") or {}).get("value", 0.0))
    checks = {
        "minimum_resolved": resolved >= policy.minimum_resolved,
        "daemon_coverage": coverage >= policy.minimum_daemon_coverage,
        "positive_daemon_improvement": float(score.get("headline_improvement_vs_daemon", 0)) > 0,
        "positive_market_improvement": float(score.get("improvement_vs_market_pre", 0)) > 0,
        "confidence_interval": (not policy.require_positive_ci_low) or float(score.get("ci_low", 0)) > 0,
        "net_value_after_fees": net > policy.minimum_net_value,
    }
    return {"policy_version": QUALIFICATION_POLICY_VERSION, "policy": asdict(policy),
            "evaluated_at_ms": int(time.time() * 1000), "qualified": all(checks.values()),
            "checks": checks, "metrics": {"resolved": resolved, "daemon_coverage": coverage,
                                            "net_value_after_fees": net}}


class ShadowExecutionAdapter:
    mode = "shadow"
    def execute(self, proposal: dict) -> dict:
        return {"adapter": "shadow-v1", "submitted": False,
                "simulated": proposal.get("gate") == "pass",
                "reason": "shadow_gate_pass" if proposal.get("gate") == "pass" else "gate_rejected"}
