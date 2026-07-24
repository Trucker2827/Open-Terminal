#!/usr/bin/env python3
"""Weekly retrain loop: keep the published quant signal's model from going stale.

The signal publisher pins a model_id; as the crypto dataset rolls forward that
model ages out of its training window. This script closes the loop:

  1. read the dataset's own calendar (env OPENTERMINAL_QLIB_DATA /
     OPENTERMINAL_QLIB_FREQ) and split it into train / valid / holdout windows,
     the holdout strictly AFTER everything the new model trains on
  2. train a fresh lightgbm on train+valid
  3. measure trailing rank-IC on the holdout for BOTH the new model and the
     incumbent (same window, same data — a fair comparison)
  4. atomically repoint active_model.json to the new model ONLY if its
     holdout rank-IC >= the incumbent's

The pointer file is how the swap reaches the publisher: signal_publisher.py
resolves model_id "active" through active_model.json on every publish cycle,
so a repoint takes effect within one cycle, no relaunch needed.

Honesty rules: a model whose IC cannot be measured is never adopted; an
incumbent that measures better (or that cannot be measured) keeps the pointer;
every decision is written into the pointer with both ICs and the reason.
The only exception is bootstrap: with no incumbent at all there is nothing to
compare, so a measurable new model is adopted and the reason says exactly that.

Usage (dataset/freq via OPENTERMINAL_QLIB_DATA / OPENTERMINAL_QLIB_FREQ):
  retrain '{"current_model_id":"lightgbm_...","eval_days":2,"valid_days":2}'
  status  '{}'
"""
import json
import os
import sys
import time

# Mirrors QlibService.MODELS_DIR without importing qlib_service (that module
# pulls in qlib/torch at import time; the publisher reads the pointer on a
# fast path and the tests must stay hermetic).
DEFAULT_MODELS_DIR = os.path.expanduser("~/.qlib/openterminal_models")
ACTIVE_MODEL_NAME = "active_model.json"
DEFAULT_MODEL_TYPE = "lightgbm"


def models_dir():
    return os.path.expanduser(
        os.environ.get("OPENTERMINAL_MODELS_DIR", DEFAULT_MODELS_DIR))


def active_model_path():
    return os.path.join(models_dir(), ACTIVE_MODEL_NAME)


def read_active(path=None):
    """Pointer content, or None when missing/unreadable — missing reads missing."""
    path = path or active_model_path()
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get("model_id") else None


def write_active_atomic(entry, path=None):
    path = path or active_model_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(entry, fh, indent=2)
    os.replace(tmp, path)
    return path


def split_windows(dates, eval_days=2, valid_days=2, min_train_days=5):
    """Carve sorted distinct dates into train/valid/holdout, holdout last.

    The holdout ("eval") window starts strictly after valid_end, so the IC
    measured there is out-of-sample for a model trained on train+valid.
    """
    eval_days, valid_days = int(eval_days), int(valid_days)
    if eval_days < 1 or valid_days < 1:
        return {"success": False, "error": "eval_days and valid_days must be >= 1"}
    dates = sorted(set(dates))
    need = eval_days + valid_days + int(min_train_days)
    if len(dates) < need:
        return {"success": False,
                "error": f"dataset spans {len(dates)} distinct dates; "
                         f"need >= {need} (train {min_train_days} + valid "
                         f"{valid_days} + holdout {eval_days})"}
    eval_dates = dates[-eval_days:]
    valid_dates = dates[-(eval_days + valid_days):-eval_days]
    train_dates = dates[:-(eval_days + valid_days)]
    return {"success": True,
            "train": {"start": train_dates[0], "end": train_dates[-1]},
            "valid": {"start": valid_dates[0], "end": valid_dates[-1]},
            "eval": {"start": eval_dates[0], "end": eval_dates[-1]}}


def decide_swap(new_rank_ic, incumbent_id, incumbent_rank_ic):
    """(swap, reason) — the compare-and-swap rule, pure and hermetically testable.

    Swap ONLY when the new model's holdout rank-IC is measured and >= the
    incumbent's. An unmeasurable new model is never adopted. An incumbent
    that cannot be measured keeps the pointer (refusing to swap is the safe
    honest state; an operator can see why). Bootstrap — no incumbent at all —
    adopts a measurable new model because there is nothing to compare against.
    """
    if new_rank_ic is None:
        return False, "new model's holdout rank-IC could not be measured — never adopt an unmeasurable model"
    if incumbent_id is None:
        return True, "no incumbent model — bootstrap adoption of the first measurable model"
    if incumbent_rank_ic is None:
        return False, (f"incumbent {incumbent_id} holdout rank-IC could not be measured — "
                       "refusing to swap without a comparison")
    if new_rank_ic >= incumbent_rank_ic:
        return True, (f"new holdout rank-IC {new_rank_ic:.6f} >= incumbent "
                      f"{incumbent_rank_ic:.6f}")
    return False, (f"new holdout rank-IC {new_rank_ic:.6f} < incumbent "
                   f"{incumbent_rank_ic:.6f} — keeping {incumbent_id}")


def apply_decision(new_model_id, new_rank_ic, incumbent_id, incumbent_rank_ic,
                   eval_window, now_ms, pointer_path=None):
    """Run decide_swap and, on swap, atomically rewrite the pointer."""
    swap, reason = decide_swap(new_rank_ic, incumbent_id, incumbent_rank_ic)
    if swap:
        write_active_atomic({
            "schema": 1, "model_id": new_model_id,
            "previous_model_id": incumbent_id, "updated_at_ms": now_ms,
            "eval_window": eval_window, "new_rank_ic": new_rank_ic,
            "incumbent_rank_ic": incumbent_rank_ic, "reason": reason,
        }, pointer_path)
    return {"swapped": swap, "reason": reason}


def _dataset_dates():
    """Distinct dates in the active dataset's calendar (env-pointed)."""
    data_dir = os.path.expanduser(
        os.environ.get("OPENTERMINAL_QLIB_DATA", "~/.qlib/qlib_data/us_data"))
    freq = os.environ.get("OPENTERMINAL_QLIB_FREQ", "day")
    cal_path = os.path.join(data_dir, "calendars", freq + ".txt")
    try:
        with open(cal_path, encoding="utf-8") as fh:
            lines = [l.strip() for l in fh if l.strip()]
    except OSError as exc:
        return None, f"cannot read dataset calendar {cal_path}: {exc}"
    if not lines:
        return None, f"dataset calendar {cal_path} is empty"
    return sorted({l[:10] for l in lines}), None


def _rank_ic(service, model_id, window):
    """(rank_ic_mean, error) from get_factor_analysis over the holdout."""
    ic = service.get_factor_analysis(model_id, "ic", window["start"], window["end"])
    if not ic.get("success"):
        return None, ic.get("error")
    value = (ic.get("results") or {}).get("Rank_IC_mean")
    if value is None:
        return None, "IC analysis returned no Rank_IC_mean"
    return float(value), None


def retrain(params):
    dates, err = _dataset_dates()
    if err:
        return {"success": False, "error": err}
    windows = split_windows(dates,
                            eval_days=params.get("eval_days", 2),
                            valid_days=params.get("valid_days", 2),
                            min_train_days=params.get("min_train_days", 5))
    if not windows["success"]:
        return {"success": False, "error": windows["error"]}

    from qlib_service import QlibService
    service = QlibService()
    trained = service.train_model(
        params.get("model_type", DEFAULT_MODEL_TYPE),
        params.get("instruments", "all"),
        windows["train"]["start"], windows["train"]["end"],
        windows["valid"]["start"], windows["valid"]["end"],
        handler_type=params.get("handler_type", "Alpha158"))
    if not trained.get("success"):
        return {"success": False, "error": f"training failed: {trained.get('error')}",
                "windows": windows}
    new_model_id = trained["model_id"]
    if not trained.get("persisted"):
        # The publisher runs in its own process; a memory-only model can
        # never serve signals, so adopting it would point at nothing.
        return {"success": False, "windows": windows, "model_id": new_model_id,
                "error": "new model was not persisted to disk — refusing to "
                         f"repoint: {trained.get('warning')}"}

    new_rank_ic, new_ic_error = _rank_ic(service, new_model_id, windows["eval"])

    pointer = read_active()
    incumbent_id = (pointer or {}).get("model_id") or params.get("current_model_id")
    incumbent_rank_ic = incumbent_ic_error = None
    if incumbent_id:
        incumbent_rank_ic, incumbent_ic_error = _rank_ic(
            service, incumbent_id, windows["eval"])

    decision = apply_decision(new_model_id, new_rank_ic, incumbent_id,
                              incumbent_rank_ic, windows["eval"],
                              int(time.time() * 1000))
    result = {
        # success = the loop ran to a decision; an unmeasurable NEW model is a
        # failed run (the retrain produced nothing adoptable).
        "success": new_rank_ic is not None,
        "model_id": new_model_id, "windows": windows,
        "new_rank_ic": new_rank_ic,
        "incumbent_model_id": incumbent_id,
        "incumbent_rank_ic": incumbent_rank_ic,
        "swapped": decision["swapped"], "reason": decision["reason"],
        "active_model_path": active_model_path(),
    }
    if new_ic_error:
        result["error"] = f"new model IC evaluation failed: {new_ic_error}"
    if incumbent_ic_error:
        result["incumbent_ic_error"] = incumbent_ic_error
    return result


def status(_params):
    pointer = read_active()
    if pointer is None:
        return {"success": True, "active_model_path": active_model_path(),
                "active": None,
                "note": "no active-model pointer yet — run retrain, or the "
                        "publisher keeps using its explicit model_id"}
    return {"success": True, "active_model_path": active_model_path(),
            "active": pointer}


def main(argv):
    if len(argv) < 2 or argv[1] not in ("retrain", "status"):
        print(json.dumps({"success": False,
                          "error": "usage: model_retrain.py retrain|status ['{json}']"}))
        return 2
    params = {}
    if len(argv) > 2:
        try:
            params = json.loads(argv[2])
        except ValueError as exc:
            print(json.dumps({"success": False, "error": f"invalid JSON: {exc}"}))
            return 2
    result = retrain(params) if argv[1] == "retrain" else status(params)
    print(json.dumps(result))
    return 0 if result.get("success") else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
