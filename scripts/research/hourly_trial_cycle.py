#!/usr/bin/env python3
"""Hash-checked paper-only hourly trial producer cycle for the local daemon."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess

import btc1h_chronos_shadow as btc_shadow
import commodity_chronos_shadow as commodity_shadow
import commodity_pyth_history as pyth
import kalshi_edge_common as common
import kxgoldh_forward_paper as gold
import kxsilverh_forward_paper as silver
import kxwtih_forward_paper as wti

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXPECTED = {
    "KXGOLDH": "3295b765f10c59f492a1fe2dfdfaa9af59d457233d7b376ca25de6c55ae66509",
    "KXSILVERH": "aac11a085224740f2a64f41ad43d4656734ef817cbf4cc35a1170dc86330a18b",
    "KXWTIH": "a63f89bf99ef4734a59fe2ed841d23be4a0465d3b4e2065251ed3c65df4e97cc",
    "BTC1H_SHADOW": "5ff50a56678ab47de95e974fde9a3ec5da6dbe9776d27a201c29a9603f78679a",
    "KXGOLDH_SHADOW": "c7291dde2454b42e587ae6450546daf7de96bab342c7a7e6038f494161059efb",
    "KXSILVERH_SHADOW": "c7390e3e6c2e8aabc7220c407927294ead013f80f56db5b845a6c90c83d13d64",
    "KXWTIH_SHADOW": "191978d0f7db557da2f26c003276c63714075b10cbdad5728f93faab8473c11e",
}


def cli_path():
    candidates = [shutil.which("openterminalcli"),
                  os.path.join(ROOT, "openmarketterminal-qt", "build", "openterminalcli")]
    return next((p for p in candidates if p and os.path.isfile(p) and os.access(p, os.X_OK)), None)


def forecast(symbol):
    executable = cli_path()
    if not executable:
        raise RuntimeError("openterminalcli not found for paper-only Chronos forecast")
    completed = subprocess.run(
        [executable, "--json", "edge", "chronos2", "forecast", symbol,
         "--horizon", "1h", "--publish", "--journal"],
        text=True, capture_output=True, timeout=420, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"Chronos {symbol} failed: {completed.stderr.strip()}")
    payload = json.loads(completed.stdout)
    if not payload.get("paper_only") or not payload.get("success"):
        raise RuntimeError(f"Chronos {symbol} did not return a successful paper forecast")
    return {"journal_id": payload.get("journal_id"), "direction": payload.get("direction")}


def verify(label, result):
    actual = (result.get("summary") or {}).get("policy_sha256")
    if actual != EXPECTED[label]:
        raise RuntimeError(f"{label} policy hash mismatch: {actual}")
    return result


def run_cycle(now_ms=None):
    import time
    now_ms = int(time.time() * 1000) if now_ms is None else int(now_ms)
    db_path = pyth.default_db()
    feed = pyth.collect(db_path=db_path, now_ms=now_ms)
    with open(common.evidence_path(commodity_shadow.REPORT_STEM + ".json"), encoding="utf-8") as handle:
        report = json.load(handle)
    report_generated = int(report.get("generated_at_ms") or 0)
    forecasts = {}
    for family, status in feed["families"].items():
        if status["status"] != "READY":
            forecasts[family] = {"status": "SKIPPED_WAITING_FOR_SETTLEMENT_FEED"}
            continue
        symbol = commodity_shadow.SPECS[family]["symbol"]
        if commodity_shadow.latest_chronos(db_path, family, report_generated):
            forecasts[family] = {"status": "USABLE_FORECAST_ALREADY_JOURNALED"}
        else:
            forecasts[family] = {"status": "FORECASTED", **forecast(symbol)}

    core = {
        "KXGOLDH": verify("KXGOLDH", gold.run_once(common.evidence_path("kxgoldh-forward-paper.json"), now_ms)),
        "KXSILVERH": verify("KXSILVERH", silver.run_once(common.evidence_path("kxsilverh-forward-paper.json"), now_ms)),
        "KXWTIH": verify("KXWTIH", wti.run_once(common.evidence_path("kxwtih-forward-paper.json"), now_ms)),
    }
    shadows = {
        family: verify(family + "_SHADOW", commodity_shadow.run_once(
            family, commodity_shadow.default_state(family), db_path, now_ms))
        for family in commodity_shadow.SPECS
    }
    shadows["BTC1H"] = verify("BTC1H_SHADOW", btc_shadow.run_once(
        btc_shadow.default_state_path(), db_path, now_ms))
    return {"event": "hourly_trial_cycle", "authority": "paper_research_only_no_order_api",
            "feed": feed, "forecasts": forecasts, "core": core, "shadows": shadows}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    print(json.dumps(run_cycle(), sort_keys=True, indent=None if args.json else 2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
