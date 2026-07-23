#!/usr/bin/env python3
"""Layer 4 of the terminal<->Quant Lab bridge: models -> engine, as EVIDENCE.

Publishes a trained model's latest cross-sectional scores into the shared
advisory-evidence directory (quant-signals.json) where the engine may read
them the same way it reads the order-book evidence: as one more input,
never as authority. The deterministic executor remains the only thing that
places orders; nothing here can trade.

Trust is measured, not assumed: every publish embeds the model's trailing
information coefficient over a recent window, and the payload's `trusted`
flag is earned (positive trailing rank-IC over enough periods) — a model
that cannot demonstrate predictive power publishes with trusted=false and
consumers are expected to treat its direction as noise.

Usage (dataset/freq via OPENTERMINAL_QLIB_DATA / OPENTERMINAL_QLIB_FREQ):
  publish '{"model_id":"lightgbm_...","ic_start":"2026-07-21","ic_end":"2026-07-22"}'
  run     '{"model_id":"...","interval_sec":300, ...}'

model_id "active" follows the weekly retrain loop's active_model.json pointer
(model_retrain.py), re-read every cycle so repoints land without a relaunch.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import evidence_file

MIN_TRUSTED_RANK_IC = 0.01
MIN_TRUSTED_PERIODS = 200
MIN_TRUSTED_TSTAT = 2.0   # Rank_ICIR * sqrt(periods): the mean must be significant, not just positive
OUTPUT_NAME = "quant-signals.json"


def evidence_path():
    return evidence_file(OUTPUT_NAME)


def resolve_model_id(params):
    """(model_id, error) — model_id "active" resolves through the retrain
    loop's active_model.json pointer, re-read on every call so a repoint by
    model_retrain.py takes effect within one publish cycle, no relaunch.
    Explicit ids pass through untouched; a missing pointer is an error, never
    a silent fallback."""
    model_id = params.get("model_id")
    if model_id != "active":
        return model_id, None
    from model_retrain import active_model_path, read_active
    pointer = read_active()
    if pointer is None:
        return None, ("model_id is 'active' but no readable active-model "
                      f"pointer at {active_model_path()} — run "
                      "model_retrain.py retrain, or pass an explicit model_id")
    return pointer["model_id"], None


def rolling_ic_window(ic_hours, now_ts=None):
    """(start_date, end_date) covering the trailing ic_hours — loop-safe,
    unlike fixed ic_start/ic_end which silently go stale in `run` mode."""
    import datetime as _dt
    now_dt = _dt.datetime.fromtimestamp(now_ts if now_ts is not None else time.time(),
                                        tz=_dt.timezone.utc)
    start_dt = now_dt - _dt.timedelta(hours=float(ic_hours))
    return str(start_dt.date()), str(now_dt.date())


def build_signal_payload(model_id, as_of, ranked, ic_results, now_ms):
    """Pure payload builder (testable without qlib).

    ranked: [{"symbol","score"}] best-first (screen output order).
    ic_results: get_factor_analysis results dict, or None when unavailable.
    """
    trusted = False
    ic_block = None
    if ic_results:
        ic_block = {"rank_ic_mean": ic_results.get("Rank_IC_mean"),
                    "rank_icir": ic_results.get("Rank_ICIR"),
                    "ic_mean": ic_results.get("IC_mean"),
                    "periods": ic_results.get("days"),
                    "positive_ic_periods": ic_results.get("positive_ic_days")}
        rank_ic = ic_results.get("Rank_IC_mean")
        rank_icir = ic_results.get("Rank_ICIR")
        periods = ic_results.get("days") or 0
        tstat = (rank_icir or 0.0) * (periods ** 0.5)
        ic_block["rank_ic_tstat"] = tstat
        trusted = bool(rank_ic is not None and rank_ic > MIN_TRUSTED_RANK_IC
                       and periods >= MIN_TRUSTED_PERIODS
                       and tstat >= MIN_TRUSTED_TSTAT)
    signals = {}
    for rank, row in enumerate(ranked, start=1):
        score = float(row["score"])
        signals[str(row["symbol"]).upper()] = {
            "score": score, "rank": rank,
            "direction": "up" if score > 0 else ("down" if score < 0 else "flat")}
    return {
        "schema": 1, "event": "quant_model_signals", "advisory_only": True,
        "authority": "none — deterministic executor remains sole authority",
        "generated_at_ms": now_ms, "model_id": model_id, "as_of": as_of,
        "trailing_ic": ic_block, "trusted": trusted,
        "trust_rule": (f"trusted iff trailing Rank_IC_mean > {MIN_TRUSTED_RANK_IC} over "
                       f">= {MIN_TRUSTED_PERIODS} periods AND Rank_ICIR*sqrt(periods) >= "
                       f"{MIN_TRUSTED_TSTAT} (significance, not just a positive mean); "
                       "untrusted directions are noise"),
        "signals": signals,
    }


def save_atomic(payload, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh)
    os.replace(tmp, path)


def publish(params):
    model_id, resolve_error = resolve_model_id(params)
    if resolve_error:
        return {"success": False, "error": resolve_error}
    if not model_id:
        return {"success": False, "error": "model_id is required"}
    from qlib_service import QlibService
    service = QlibService()
    screen = service.screen(model_id, params.get("date"),
                            top=int(params.get("top", 50)), bottom=0)
    if not screen.get("success"):
        return {"success": False, "error": f"scoring failed: {screen.get('error')}"}
    ic_results = None
    ic_start, ic_end = params.get("ic_start"), params.get("ic_end")
    if params.get("ic_hours") and not (ic_start and ic_end):
        ic_start, ic_end = rolling_ic_window(params["ic_hours"])
    if ic_start and ic_end:
        ic = service.get_factor_analysis(model_id, "ic", ic_start, ic_end)
        if ic.get("success"):
            ic_results = ic.get("results")
    payload = build_signal_payload(model_id, screen.get("as_of"), screen.get("top", []),
                                   ic_results, int(time.time() * 1000))
    save_atomic(payload, evidence_path())
    return {"success": True, "path": evidence_path(), "as_of": payload["as_of"],
            "trusted": payload["trusted"], "signals": len(payload["signals"]),
            "trailing_ic": payload["trailing_ic"]}


def main(argv):
    if len(argv) < 2 or argv[1] not in ("publish", "run"):
        print(json.dumps({"success": False,
                          "error": "usage: signal_publisher.py publish|run ['{json}']"}))
        return 2
    params = {}
    if len(argv) > 2:
        try:
            params = json.loads(argv[2])
        except ValueError as exc:
            print(json.dumps({"success": False, "error": f"invalid JSON: {exc}"}))
            return 2
    if argv[1] == "publish":
        result = publish(params)
        print(json.dumps(result))
        return 0 if result.get("success") else 1
    interval = max(30, int(params.get("interval_sec", 300)))
    while True:
        result = publish(params)
        print(json.dumps({"published": result.get("success"),
                          "trusted": result.get("trusted"),
                          "error": result.get("error")}), flush=True)
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
