#!/usr/bin/env python3
"""Honesty regression: the deceptive AI Quant Lab tools must FAIL LOUDLY, never fabricate.

The C++ caller (AIQuantLabService::run_python) trusts the PROCESS EXIT CODE, not the
JSON `success` field. So this test verifies the load-bearing contract directly: the
no-demo path of each synthetic-data tool must

  (1) exit with a NON-ZERO process exit code, and
  (2) emit JSON with success:false and the expected error_kind  (no fabricated metrics).

RL / Meta / Feature-engineering are exercised as SUBPROCESSES at their real CLI entry
points (exactly how the app runs them), so the exit-code contract is actually observed.
Advanced Models is exercised in-process because its model object only lives in memory
(create_model persists metadata only), so the no_input gate is only reachable in the
same process that created the model.

Run with an interpreter that has numpy+pandas (and torch/sklearn for full coverage):
  <pybin> scripts/ai_quant_lab/test_honesty_quantlab.py ; echo EXIT=$?
Expected: [honesty-quantlab] PASS  /  EXIT=0
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
PYBIN = sys.executable

_failures = []


def check(name, cond):
    if cond:
        print(f"[honesty-quantlab] ok: {name}")
    else:
        print(f"[honesty-quantlab] FAIL: {name}")
        _failures.append(name)


def _last_json(stdout):
    """Return the LAST top-level JSON value in stdout.

    Handles both forms the scripts emit: RL streams single-line progress/result
    events (multiple JSON values), while Meta/Advanced pretty-print a single object
    across many lines (json.dumps indent=2). A raw_decode scan covers both.
    """
    dec = json.JSONDecoder()
    s = stdout
    i = 0
    last = None
    n = len(s)
    while i < n:
        while i < n and s[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        try:
            obj, end = dec.raw_decode(s, i)
        except ValueError:
            i += 1
            continue
        last = obj
        i = end
    return last


def run_script(script, args):
    """Run a quant-lab script at its real CLI entry point. Returns (exit_code, parsed_json)."""
    proc = subprocess.run(
        [PYBIN, os.path.join(HERE, script), *args],
        capture_output=True, text=True, cwd=HERE,
    )
    return proc.returncode, _last_json(proc.stdout)


# NEW CONTRACT: "no demo" no longer means FAIL. It means the tool trains on REAL
# qlib data (success, demo:false, source:"qlib") for a covered ticker — OR returns an
# honest no_input error for an uncovered ticker. The invariant preserved is that a
# success result is NEVER backed by np.random unless it is explicitly tagged demo:true.

def _demo_is_off(payload):
    return isinstance(payload, dict) and payload.get("demo") in (False, None)


# --------------------------------------------------------------------------
# Q1 — RL Trading.
# --------------------------------------------------------------------------
# (a) default (no demo) on a covered ticker → real qlib success.
rc, payload = run_script("qlib_rl.py", ["train", json.dumps({"ticker": "AAPL", "episodes": 1})])
check("RL default(AAPL) exits zero", rc == 0)
check("RL default(AAPL) is real qlib success (demo off, source=qlib)",
      isinstance(payload, dict) and payload.get("success") is True
      and _demo_is_off(payload) and payload.get("source") in ("yahoo", "qlib"))
# (b) demo=true → still SYNTHETIC, tagged demo:true.
rc, payload = run_script("qlib_rl.py",
                         ["train", json.dumps({"ticker": "AAPL", "episodes": 1, "demo": True})])
check("RL demo=true is synthetic (demo:true, no qlib source)",
      isinstance(payload, dict) and payload.get("success") is True
      and payload.get("demo") is True and payload.get("source") not in ("yahoo", "qlib"))
# (c) uncovered/bad ticker → honest no_input, non-zero exit, NEVER synthetic.
rc, payload = run_script("qlib_rl.py", ["train", json.dumps({"ticker": "ZZZZZ_NOPE", "episodes": 1})])
check("RL bad-ticker exits non-zero", rc != 0)
check("RL bad-ticker reports no_input (no silent synthetic)",
      isinstance(payload, dict) and payload.get("success") is False
      and payload.get("error_kind") == "no_input")

# --------------------------------------------------------------------------
# Q3 — Meta Learning: run_selection.
# --------------------------------------------------------------------------
# (a) default(AAPL) → real qlib success.
rc, payload = run_script("qlib_meta_learning.py",
                         ["run_selection", json.dumps({"model_ids": ["random_forest"], "ticker": "AAPL"})])
check("Meta default(AAPL) exits zero", rc == 0)
check("Meta default(AAPL) is real qlib success (demo off, source=qlib)",
      isinstance(payload, dict) and payload.get("success") is True
      and _demo_is_off(payload) and payload.get("source") in ("yahoo", "qlib"))
# (b) demo=true → synthetic, tagged.
rc, payload = run_script(
    "qlib_meta_learning.py",
    ["run_selection", json.dumps({"model_ids": ["random_forest"], "demo": True})])
check("Meta demo=true is synthetic (demo:true, no qlib source)",
      isinstance(payload, dict) and payload.get("success") is True
      and payload.get("demo") is True and payload.get("source") not in ("yahoo", "qlib"))
# (c) bad ticker → honest no_input.
rc, payload = run_script(
    "qlib_meta_learning.py",
    ["run_selection", json.dumps({"model_ids": ["random_forest"], "ticker": "ZZZZZ_NOPE"})])
check("Meta bad-ticker exits non-zero", rc != 0)
check("Meta bad-ticker reports no_input",
      isinstance(payload, dict) and payload.get("success") is False
      and payload.get("error_kind") == "no_input")

# Q3 (cont.) — the SECOND data path: hyperparameter tuning.
# (a) default(AAPL) → real qlib success.
rc, payload = run_script(
    "qlib_meta_learning.py",
    ["tune_hyperparameters", json.dumps({"model_id": "random_forest", "ticker": "AAPL",
                                         "param_grid": {"n_estimators": [10, 50]}})])
check("Meta tune default(AAPL) exits zero", rc == 0)
check("Meta tune default(AAPL) is real qlib success (demo off, source=qlib)",
      isinstance(payload, dict) and payload.get("success") is True
      and _demo_is_off(payload) and payload.get("source") in ("yahoo", "qlib"))
# (b) bad ticker → honest no_input.
rc, payload = run_script(
    "qlib_meta_learning.py",
    ["tune_hyperparameters", json.dumps({"model_id": "random_forest", "ticker": "ZZZZZ_NOPE",
                                         "param_grid": {"n_estimators": [10, 50]}})])
check("Meta tune bad-ticker exits non-zero", rc != 0)
check("Meta tune bad-ticker reports no_input",
      isinstance(payload, dict) and payload.get("success") is False
      and payload.get("error_kind") == "no_input")

# --------------------------------------------------------------------------
# Q6 — Feature engineering: the placeholder expression engine must refuse, not
#       silently return `close`.
# --------------------------------------------------------------------------
rc, payload = run_script(
    "qlib_feature_engineering.py",
    ["evaluate_expression", json.dumps({"data": {"close": [1, 2, 3]},
                                        "expression": "Mean($close, 20)"})])
check("FeatureEng expression exits non-zero", rc != 0)
check("FeatureEng expression reports not_implemented",
      isinstance(payload, dict) and payload.get("success") is False
      and payload.get("error_kind") == "not_implemented")

# --------------------------------------------------------------------------
# Q2 — Advanced Models. Verified IN-PROCESS: _save_state persists metadata only (no
#       live torch model), so a `create` then a separate-process `train <id>` can't
#       share the model object. We create a real model and exercise the data paths in
#       the same process — exactly the contract the gate guards.
# --------------------------------------------------------------------------
try:
    import qlib_advanced_models as qam
    if not getattr(qam, "TORCH_AVAILABLE", False):
        print("[honesty-quantlab] skip: Advanced Models (torch not available)")
    else:
        mgr = qam.AdvancedModelsManager()
        # (a) default(AAPL) on a REAL created model → real qlib success.
        cr = mgr.create_model("lstm", "honesty_real", {"input_size": 10})
        check("AdvModels create_model works (SimpleLSTM defined)", cr.get("success") is True)
        r = mgr.train_model("honesty_real", epochs=1, ticker="AAPL")
        check("AdvModels default(AAPL) is real qlib success (demo off, source=qlib)",
              isinstance(r, dict) and r.get("success") is True
              and r.get("demo") in (False, None) and r.get("source") in ("yahoo", "qlib"))
        # (b) demo=true → synthetic, tagged demo:true.
        mgr.create_model("lstm", "honesty_demo", {"input_size": 10})
        rd = mgr.train_model("honesty_demo", epochs=1, demo=True)
        check("AdvModels demo=true is synthetic (demo:true, no qlib source)",
              isinstance(rd, dict) and rd.get("success") is True
              and rd.get("demo") is True and rd.get("source") != "qlib")
        # (c) uncovered/bad ticker → honest no_input, NEVER silent synthetic.
        mgr.create_model("lstm", "honesty_bad", {"input_size": 10})
        rb = mgr.train_model("honesty_bad", epochs=1, ticker="ZZZZZ_NOPE")
        check("AdvModels bad-ticker refuses with no_input (no fake loss)",
              isinstance(rb, dict) and rb.get("success") is False
              and rb.get("error_kind") == "no_input")
except ImportError as e:
    print(f"[honesty-quantlab] skip: Advanced Models import failed ({e})")

# Q2 (cont.) — verify the Advanced Models main() exit-code wiring through a real
# subprocess. Any failing command must drive a NON-ZERO process exit (the contract
# the C++ caller trusts). 'train <missing_id>' fails the model lookup before any
# torch construction, so it's environment-independent.
rc, payload = run_script("qlib_advanced_models.py", ["train", "no_such_model", "{}"])
check("AdvModels main() exits non-zero on failure",
      rc != 0 and isinstance(payload, dict) and payload.get("success") is False)

# --------------------------------------------------------------------------
if _failures:
    print(f"[honesty-quantlab] FAILED ({len(_failures)}): {', '.join(_failures)}")
    sys.exit(1)
print("[honesty-quantlab] PASS")
sys.exit(0)
