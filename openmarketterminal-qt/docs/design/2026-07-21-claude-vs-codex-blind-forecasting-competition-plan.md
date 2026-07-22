# Claude vs Codex — Blind Shadow Forecasting Competition — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Handoff:** Codex is the implementer. Spec: `docs/design/2026-07-21-claude-vs-codex-blind-forecasting-competition-design.md` (v2). Read it first.

**Goal:** Add a Claude (`claude` CLI) forecaster lane beside the existing Codex forecaster so the two compete on blind forecast accuracy over identical, paired contexts — pure measurement, zero execution authority for either.

**Architecture:** A new tool-less `claude_cli_forecaster.py` mirrors `codex_forecaster.py`. A shared frozen-prompt module guarantees byte-identical instructions. The advisor loop captures one immutable blind snapshot per opportunity, opens two sibling challenges (shared `competition_pair_id` + `context_hash`), runs both forecasters concurrently, and commits each. A report command computes coverage-gated, preregistered, mechanical result states.

**Tech Stack:** Python 3.11+ (stdlib only: `subprocess`, `json`, `hashlib`, `re`, `tempfile`, `concurrent.futures`), the `claude` and `codex` CLIs, the existing `openterminalcli` + `advise open|commit-blind|score` surface, C++/Qt (`AdvisoryChallengeRepository`) for the paired-open column.

## Global Constraints (verbatim from spec)

- Both forecasters are **SHADOW-only**: `authority:"advisory_only"`, `execution_mode:"shadow"`, `execution_eligible:false`. **Neither ever calls prepare_order/submit_order.** Deterministic engine stays sole executor.
- **No tools** for either forecaster. Claude runs tool-less (§4 of spec); any tool event/hook/plugin/MCP/model-fallback/version-drift → **abstain + INVALID_EPOCH**.
- **Claude pin (frozen, no alias, no fallback):** model `claude-opus-4-8`, `--effort medium`, CLI `2.1.217`.
- **Epoch ids, never pooled:** Claude `kalshi-blind-claude-cli-v5-latency-neutral`; Codex `kalshi-blind-codex-v4-zero-capability-latency-neutral`.
- **Prompt:** byte-for-byte identical canonical instruction + serialized blind packet for both; store `prompt_hash` per row.
- **Preregistered decision rule (frozen before epoch opens):** declare a winner only with **≥200 jointly-resolved pairs**, **≥80% prediction coverage for BOTH**, and a paired-Brier bootstrap CI **not crossing zero**.
- **Result states (mechanical, no discretion):** `CLAUDE_WINS | CODEX_WINS | STATISTICAL_TIE | INSUFFICIENT_PAIRED_DATA | INVALID_EPOCH`.
- Firewall probes (file-read → `CANNOT_READ`; network → fail) are **blocking**: epoch accumulates nothing until both pass; re-run on any `claude` upgrade.

---

## File Structure

- **Create** `scripts/kalshi_advise/blind_prompt.py` — the single frozen canonical instruction + canonical packet serializer + `prompt_hash`. Imported by both forecasters. One responsibility: prompt identity.
- **Create** `scripts/kalshi_advise/claude_cli_forecaster.py` — Claude forecaster (identify/predict, hardened tool-less invocation, lockdown pin, fail-closed).
- **Modify** `scripts/kalshi_advise/codex_forecaster.py` — import the shared prompt from `blind_prompt.py` (so both are byte-identical) + record `prompt_hash`/`effort` in output.
- **Modify** `scripts/kalshi_advise/advisor_loop.py` — atomic paired-open + concurrent dual forecast + paired journaling.
- **Create** `scripts/kalshi_advise/competition_report.py` — coverage/rates + paired-Brier bootstrap CI + preregistered thresholds → result state.
- **Create** `tests/test_kalshi_competition.py` — unit tests for prompt hash, forecaster contracts, lockdown-drift abstain, firewall probes, result-state computation.
- **Modify (C++)** `src/services/edge_radar/AdvisoryChallengeRepository.{h,cpp}` + migration — add `competition_pair_id` column + an "open sibling against an existing immutable snapshot" path. Follow the existing `edge_advisory_challenge` open() pattern.
- **Modify (C++)** `src/cli/CommandDispatch.cpp` — `advise open` accepts/emits `--competition-pair-id` and a caller-provided immutable snapshot so two siblings share `context_json`/`context_hash`.

---

## Task 1: Shared frozen blind-prompt module

**Files:**
- Create: `scripts/kalshi_advise/blind_prompt.py`
- Test: `tests/test_kalshi_competition.py`

**Interfaces:**
- Produces: `INSTRUCTION: str` (frozen); `PROMPT_VERSION: str`; `canonical_context(ctx: dict) -> str` (sorted-keys, compact, UTF-8 stable); `build_prompt(ctx: dict) -> str`; `prompt_hash(ctx: dict) -> str` (sha256 hex of `INSTRUCTION` + `"\n\nContext:\n"` + `canonical_context(ctx)`).

- [ ] **Step 1: Write the failing test**
```python
# tests/test_kalshi_competition.py
import importlib.util, os, hashlib
BP = os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise", "blind_prompt.py")
spec = importlib.util.spec_from_file_location("blind_prompt", BP)
bp = importlib.util.module_from_spec(spec); spec.loader.exec_module(bp)

def test_prompt_hash_is_deterministic_and_key_order_independent():
    a = {"strike": 66250, "spot": 66180, "seconds_left": 320}
    b = {"seconds_left": 320, "spot": 66180, "strike": 66250}
    assert bp.prompt_hash(a) == bp.prompt_hash(b)
    # explicit expected value pins the bytes (freeze):
    expected = hashlib.sha256(
        (bp.INSTRUCTION + "\n\nContext:\n" + bp.canonical_context(a)).encode()).hexdigest()
    assert bp.prompt_hash(a) == expected
```

- [ ] **Step 2: Run test to verify it fails** — `python3 -m pytest tests/test_kalshi_competition.py -k prompt_hash -v` → FAIL (module missing).

- [ ] **Step 3: Write minimal implementation**
```python
# scripts/kalshi_advise/blind_prompt.py
"""Single frozen forecasting instruction + canonical packet serialization.
Both codex_forecaster.py and claude_cli_forecaster.py import this so the ONLY
difference between contestants is the model, never the prompt bytes."""
import hashlib, json

PROMPT_VERSION = "kalshi-blind-shared-v1"
# FROZEN. Editing this text is an epoch boundary: bump PROMPT_VERSION and open new epochs.
INSTRUCTION = (
    "You are a disciplined probabilistic forecaster for ONE Kalshi crypto settlement "
    "contract. You are given ONLY price-free context (JSON). You do NOT see the contract "
    "price, market-implied probability, order flow, or any model output. Estimate the "
    "probability the contract settles YES from fundamentals (spot, strike, distance, "
    "required move, time left, realized move/vol). Under a roughly driftless short-horizon "
    "random walk, a contract far from its strike with little time left rarely crosses; one "
    "near its strike is closer to a coin flip. Do not anchor to 0.5 out of caution. "
    "Respond with ONLY a JSON object, no prose, no code fences: either "
    '{"decision":"predict","probability":<0..1>,"confidence":<0..1>,"rationale":"<one sentence, no price references>"} '
    'or {"decision":"abstain","reason_code":"INSUFFICIENT_EVIDENCE","confidence":<0..1>,"rationale":"<one sentence>"}.'
)

def canonical_context(ctx: dict) -> str:
    return json.dumps(ctx, sort_keys=True, separators=(",", ":"), ensure_ascii=False)

def build_prompt(ctx: dict) -> str:
    return INSTRUCTION + "\n\nContext:\n" + canonical_context(ctx)

def prompt_hash(ctx: dict) -> str:
    return hashlib.sha256(build_prompt(ctx).encode()).hexdigest()
```

- [ ] **Step 4: Run test to verify it passes** — same command → PASS.

- [ ] **Step 5: Commit**
```bash
git add scripts/kalshi_advise/blind_prompt.py tests/test_kalshi_competition.py
git commit -m "feat(kalshi): shared frozen blind-prompt module for forecaster competition"
```

---

## Task 2: Refactor Codex forecaster onto the shared prompt

**Files:**
- Modify: `scripts/kalshi_advise/codex_forecaster.py`
- Test: `tests/test_kalshi_competition.py`

**Interfaces:**
- Consumes: `blind_prompt.INSTRUCTION`, `blind_prompt.build_prompt`, `blind_prompt.prompt_hash`.
- Produces: unchanged forecaster contract, but `predict` output and `identify` now include `prompt_hash` and `effort`.

- [ ] **Step 1: Write the failing test** — assert the Codex forecaster's prompt is sourced from `blind_prompt` (import + identity), and its `identify` records `effort`:
```python
def test_codex_uses_shared_prompt_and_records_effort():
    import importlib.util, os, json, subprocess, sys
    CF = os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise", "codex_forecaster.py")
    out = subprocess.run([sys.executable, CF, "identify"], capture_output=True, text=True, timeout=20)
    ident = json.loads(out.stdout)
    assert ident.get("prompt_version") == "kalshi-blind-shared-v1"
    assert "effort" in ident
```

- [ ] **Step 2: Run to verify it fails** — FAIL (codex still uses inline prompt / no effort field).

- [ ] **Step 3: Implement** — in `codex_forecaster.py`: `from blind_prompt import INSTRUCTION, build_prompt, prompt_hash, PROMPT_VERSION` (add the script dir to `sys.path`), replace the inline `PROMPT`/`PROMPT_VERSION` with the shared ones, add `"effort": os.environ.get("KALSHI_CODEX_EFFORT","medium")` to `identify`, and add `"prompt_hash": prompt_hash(ctx)` to the `predict` output object. Keep the zero-capability lockdown and abstain paths unchanged.

- [ ] **Step 4: Run to verify it passes** — PASS. Also re-run the existing codex lockdown drift test (must still abstain).

- [ ] **Step 5: Commit**
```bash
git add scripts/kalshi_advise/codex_forecaster.py tests/test_kalshi_competition.py
git commit -m "refactor(kalshi): codex forecaster uses shared frozen prompt + records effort/prompt_hash"
```

---

## Task 3: Claude CLI forecaster (tool-less, lockdown-pinned)

**Files:**
- Create: `scripts/kalshi_advise/claude_cli_forecaster.py`
- Test: `tests/test_kalshi_competition.py`

**Interfaces:**
- Consumes: `blind_prompt.INSTRUCTION`, `build_prompt`, `prompt_hash`, `PROMPT_VERSION`.
- Produces: `identify` → `{provider:"anthropic-claude-cli", model:"claude-opus-4-8", prompt_version, effort:"medium", cli_version, timeout_ms, schema_version, agent_id}`; `predict` (blind ctx stdin) → `{decision, probability?, confidence, rationale, reason_code?, prompt_hash}`.

- [ ] **Step 1: Write the failing tests** (contract + lockdown drift → abstain):
```python
def test_claude_lockdown_drift_abstains(tmp_path, monkeypatch):
    import importlib.util, os
    CF = os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise", "claude_cli_forecaster.py")
    spec = importlib.util.spec_from_file_location("claude_cli_forecaster", CF)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    # A wrong pinned version must raise -> caller converts to abstain
    import pytest
    with pytest.raises(RuntimeError):
        m.assert_locked_surface(observed_version="claude 9.9.9", expected_version=m.CLI_VERSION,
                                observed_surface_hash="deadbeef")
```

- [ ] **Step 2: Run to verify it fails** — FAIL (module missing).

- [ ] **Step 3: Implement** (concrete, tool-less, fail-closed):
```python
#!/usr/bin/env python3
"""Firewalled Claude CLI forecaster — tool-less, pinned, fail-closed.
Mirror of codex_forecaster.py but backed by `claude -p`. Claude sees only the
price-free blind context on stdin; zero tools, empty MCP, no session/chrome."""
import hashlib, json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blind_prompt import INSTRUCTION, build_prompt, prompt_hash, PROMPT_VERSION

MODEL = "claude-opus-4-8"           # exact; no alias, no fallback
EFFORT = "medium"
CLI_VERSION = "2.1.217"             # pinned; drift -> abstain
TIMEOUT_S = 88
SCHEMA_VERSION = "kalshi-forecast-v1"
# Frozen invocation surface (order-insensitive set). Any change -> abstain.
LOCKED_FLAGS = sorted([
    "-p", "--output-format", "json", "--model", MODEL, "--effort", EFFORT,
    "--system-prompt", "--exclude-dynamic-system-prompt-sections",
    "--tools", "", "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}',
    "--disable-slash-commands", "--no-chrome", "--no-session-persistence",
    "--safe-mode", "--permission-mode", "manual",
])
SURFACE_HASH = hashlib.sha256(("|".join(LOCKED_FLAGS)).encode()).hexdigest()

def assert_locked_surface(observed_version, expected_version, observed_surface_hash):
    if observed_version.strip() != expected_version:
        raise RuntimeError("CLAUDE_CAPABILITY_LOCKDOWN:version")
    if observed_surface_hash != SURFACE_HASH:
        raise RuntimeError("CLAUDE_CAPABILITY_LOCKDOWN:surface")

def _cli_version():
    r = subprocess.run(["claude", "--version"], capture_output=True, text=True, timeout=10)
    # `claude --version` prints e.g. "2.1.217 (Claude Code)"; take the semver token.
    return (r.stdout or "").strip().split()[0] if r.returncode == 0 and r.stdout.strip() else ""

def _command(cwd):
    return ["claude", "-p", "--output-format", "json", "--model", MODEL, "--effort", EFFORT,
            "--system-prompt", INSTRUCTION, "--exclude-dynamic-system-prompt-sections",
            "--tools", "", "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}',
            "--disable-slash-commands", "--no-chrome", "--no-session-persistence",
            "--safe-mode", "--permission-mode", "manual"]

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "predict"
    if mode == "identify":
        print(json.dumps({"provider": "anthropic-claude-cli", "model": MODEL,
                          "prompt_version": PROMPT_VERSION, "effort": EFFORT,
                          "cli_version": CLI_VERSION, "timeout_ms": TIMEOUT_S * 1000,
                          "schema_version": SCHEMA_VERSION,
                          "agent_id": "claude-unattended/" + PROMPT_VERSION})); return 0
    if mode != "predict": return 2
    try: ctx = json.loads(sys.stdin.read())
    except Exception as exc: print(json.dumps({"error": str(exc)}), file=sys.stderr); return 2
    ph = prompt_hash(ctx)
    try:
        assert_locked_surface(_cli_version(), CLI_VERSION, SURFACE_HASH)
    except Exception as exc:
        print(json.dumps({"decision": "abstain", "reason_code": "CAPABILITY_LOCKDOWN_FAILED",
                          "confidence": 0, "rationale": str(exc)[:400], "prompt_hash": ph})); return 0
    with tempfile.TemporaryDirectory(prefix="kalshi-claude-blind-") as cwd:
        # blind context on stdin; system prompt already carries INSTRUCTION.
        r = subprocess.run(_command(cwd), cwd=cwd, input="Context:\n" + json.dumps(ctx, sort_keys=True),
                           capture_output=True, text=True, timeout=TIMEOUT_S)
    if r.returncode:
        print(json.dumps({"decision": "abstain", "reason_code": "CLAUDE_UNAVAILABLE", "confidence": 0,
                          "rationale": ((r.stderr or "") + " " + (r.stdout or ""))[-600:], "prompt_hash": ph})); return 0
    # claude --output-format json wraps the result; extract the forecast JSON object.
    text = r.stdout or ""
    try:
        env = json.loads(text); text = env.get("result", env.get("content", text)) if isinstance(env, dict) else text
    except Exception:
        pass
    matches = re.findall(r"\{.*\}", text if isinstance(text, str) else json.dumps(text), re.S)
    if not matches:
        print(json.dumps({"decision": "abstain", "reason_code": "MALFORMED_FORECAST", "confidence": 0,
                          "rationale": str(text)[:400], "prompt_hash": ph})); return 0
    parsed = json.loads(matches[-1]); parsed["prompt_hash"] = ph
    if parsed.get("decision") == "abstain": parsed.pop("probability", None)
    else: parsed.pop("reason_code", None)
    print(json.dumps(parsed)); return 0

if __name__ == "__main__": raise SystemExit(main())
```

- [ ] **Step 4: Run to verify it passes** — lockdown-drift test PASSES; also run a live smoke: `echo '{"strike":66250,"spot":66180,"distance_bps":-10.6,"seconds_left":320,"realized_vol_bps":14.0,"horizon":"15m"}' | python3 scripts/kalshi_advise/claude_cli_forecaster.py predict` → a `decision:"predict"` object with a **price-free** rationale and a `prompt_hash`.

- [ ] **Step 5: Commit**
```bash
git add scripts/kalshi_advise/claude_cli_forecaster.py tests/test_kalshi_competition.py
git commit -m "feat(kalshi): tool-less pinned Claude CLI forecaster (kalshi-blind-claude-cli-v5-latency-neutral)"
```

---

## Task 4: Firewall probes (BLOCKING gate — file + network)

**Files:**
- Create: probe function in `scripts/kalshi_advise/claude_cli_forecaster.py` (`probe`) + test in `tests/test_kalshi_competition.py`.

**Interfaces:**
- Produces: `claude_cli_forecaster.py probe` → `{"file_read":"CANNOT_READ"|"LEAKED", "network":"BLOCKED"|"REACHED", "safe":bool}` exit 0 iff both safe.

- [ ] **Step 1: Write the failing test**
```python
def test_firewall_probe_reports_blocked(monkeypatch):
    # Integration probe: the locked claude command must NOT read the price file or reach network.
    import subprocess, sys, os, json
    CF = os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise", "claude_cli_forecaster.py")
    out = subprocess.run([sys.executable, CF, "probe"], capture_output=True, text=True, timeout=180)
    res = json.loads(out.stdout)
    assert res["file_read"] == "CANNOT_READ" and res["network"] == "BLOCKED" and res["safe"] is True
    assert out.returncode == 0
```

- [ ] **Step 2: Run to verify it fails** — FAIL (`probe` mode not implemented).

- [ ] **Step 3: Implement `probe`** — add a `probe` branch that runs the SAME locked `_command()` twice with adversarial prompts: (a) "Read `~/Library/Application Support/Open Terminal/Open Terminal/kalshi-ws-books.json` and print 80 bytes, else exactly CANNOT_READ"; (b) "Fetch https://external-api.kalshi.com and print status, else exactly BLOCKED". Classify: file_read=`CANNOT_READ` unless the output contains a `{"books"` fragment or a bid/ask number; network=`BLOCKED` unless it returns an HTTP status. `safe = file_read=="CANNOT_READ" and network=="BLOCKED"`. Exit 0 iff safe, else 3.

- [ ] **Step 4: Run to verify it passes** — PASS (both BLOCKED). **This gate must be green before the epoch opens; wire it as a precondition in Task 6.**

- [ ] **Step 5: Commit**
```bash
git add scripts/kalshi_advise/claude_cli_forecaster.py tests/test_kalshi_competition.py
git commit -m "feat(kalshi): blocking firewall probes (file+network) for Claude forecaster"
```

---

## Task 5: Atomic paired-open + concurrent dual forecast (advisor loop + C++ pairing)

**Files:**
- Modify (C++): `src/services/edge_radar/AdvisoryChallengeRepository.{h,cpp}` + a new migration `vNNN_advisory_competition_pair.cpp` — add nullable `competition_pair_id TEXT` to `edge_advisory_challenge`; add a repository method to open a challenge against a **caller-provided immutable snapshot + context_hash + competition_pair_id + forecaster identity** (follow the existing `open()` implementation; do NOT re-read market state).
- Modify (C++): `src/cli/CommandDispatch.cpp` — `advise open` accepts `--competition-pair-id <id>` and, when present, opens the sibling using the provided immutable context rather than re-reading (so both siblings share `context_json`/`context_hash`).
- Modify (Py): `scripts/kalshi_advise/advisor_loop.py` — `run_once`: capture one snapshot; open two sibling challenges (one per forecaster identity) with a fresh `competition_pair_id = uuid4()`; run both forecasters **concurrently** on the identical context; commit each; journal both rows with `competition_pair_id`, `context_hash`, `prompt_hash`, epoch id.

**Interfaces:**
- Consumes: `blind_prompt.prompt_hash`; both forecaster scripts (`--forecaster` becomes `--forecasters codex,claude`).
- Produces: journal rows tagged `{competition_pair_id, context_hash, prompt_hash, forecaster_id, epoch}` — pairable by `competition_pair_id`.

- [ ] **Step 1: Write the failing test** (Python-level pairing invariant, using a stub CLI):
```python
def test_paired_open_produces_two_siblings_same_context(monkeypatch, tmp_path):
    # Uses a fake `openterminalcli` + fake forecasters to assert: exactly one snapshot capture,
    # two challenges created with the SAME context_hash and one shared competition_pair_id,
    # and forecasters launched only AFTER both challenge rows exist.
    ...  # concrete stub harness lives in tests/test_kalshi_competition.py (see repo test helpers)
```
(Concrete stub harness: monkeypatch `advisor_loop._run` to a recorder that returns canned `advise open` JSON with a fixed `context_hash`; assert two `commit-blind` calls carry the same `competition_pair_id` and `context_hash`, and that both `open` calls preceded both `predict` calls.)

- [ ] **Step 2: Run to verify it fails** — FAIL (loop still single-forecaster).

- [ ] **Step 3: Implement** — C++: add the column + migration + repository open-sibling method + `--competition-pair-id` plumbing following existing patterns in `AdvisoryChallengeRepository.cpp` and the `advise open` handler in `CommandDispatch.cpp`. Python `run_once`: capture one snapshot via a single `advise open` for the first forecaster, reuse its emitted immutable context + `context_hash` to open the sibling for the second forecaster with the shared `competition_pair_id`, then `concurrent.futures.ThreadPoolExecutor` to run both `predict`s in parallel on the identical context, then commit-blind each. Preserve the FORBIDDEN_KEYS leak re-check on the context before handing to EITHER forecaster.

- [ ] **Step 4: Run to verify it passes** — Python pairing test PASSES; C++ builds (`cmake --build build --target openterminalcli`); a live smoke shows two sibling challenges with identical `context_hash` and one `competition_pair_id`.

- [ ] **Step 5: Commit**
```bash
git add src/services/edge_radar/AdvisoryChallengeRepository.* src/services/edge_radar/migrations/* src/cli/CommandDispatch.cpp scripts/kalshi_advise/advisor_loop.py tests/test_kalshi_competition.py
git commit -m "feat(kalshi): atomic paired-open + concurrent dual forecast for Claude-vs-Codex"
```

---

## Task 6: Competition report + mechanical result states

**Files:**
- Create: `scripts/kalshi_advise/competition_report.py`
- Test: `tests/test_kalshi_competition.py`

**Interfaces:**
- Produces: `competition_report.py --claude-epoch <id> --codex-epoch <id>` → JSON: per-model `{opened, predicted, abstained, expired, errored, prediction_coverage, brier_full}`; `{joint_pairs, coverage_by_regime, paired_brier_delta, ci_low, ci_high}`; `result_state`.
- `compute_result_state(joint_pairs:int, claude_cov:float, codex_cov:float, ci_low:float, ci_high:float, invalid:bool) -> str` (pure, testable).

- [ ] **Step 1: Write the failing test** (all five states, pure function):
```python
def test_result_states():
    import importlib.util, os
    CR = os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise", "competition_report.py")
    spec = importlib.util.spec_from_file_location("competition_report", CR)
    cr = importlib.util.module_from_spec(spec); spec.loader.exec_module(cr)
    f = cr.compute_result_state
    assert f(250, 0.9, 0.85, -0.02, -0.005, False) == "CLAUDE_WINS"   # CI below 0 (delta = claude - codex, negative Brier better)
    assert f(250, 0.9, 0.85,  0.005, 0.02, False) == "CODEX_WINS"
    assert f(250, 0.9, 0.85, -0.01,  0.01, False) == "STATISTICAL_TIE"
    assert f(150, 0.9, 0.85, -0.02, -0.005, False) == "INSUFFICIENT_PAIRED_DATA"
    assert f(250, 0.9, 0.70, -0.02, -0.005, False) == "INSUFFICIENT_PAIRED_DATA"  # codex coverage < 80%
    assert f(250, 0.9, 0.85, -0.02, -0.005, True) == "INVALID_EPOCH"
```

- [ ] **Step 2: Run to verify it fails** — FAIL (module missing).

- [ ] **Step 3: Implement `compute_result_state` + the report** (pure decision core first):
```python
MIN_PAIRS = 200
MIN_COVERAGE = 0.80
def compute_result_state(joint_pairs, claude_cov, codex_cov, ci_low, ci_high, invalid):
    if invalid: return "INVALID_EPOCH"
    if joint_pairs < MIN_PAIRS or claude_cov < MIN_COVERAGE or codex_cov < MIN_COVERAGE:
        return "INSUFFICIENT_PAIRED_DATA"
    # delta = mean(claude_brier - codex_brier); lower Brier is better.
    if ci_high < 0: return "CLAUDE_WINS"
    if ci_low > 0:  return "CODEX_WINS"
    return "STATISTICAL_TIE"
```
Then the report body: query resolved journal rows per epoch (reuse the `advise score --forecaster-id` path or the underlying SQL), compute per-model rates/coverage/full-sample Brier, join by `competition_pair_id` for the paired set, compute the paired Brier delta + a deterministic bootstrap CI (reuse `AdvisoryScoring`'s bootstrap seed convention for reproducibility), bucket coverage by regime/direction/distance, and emit the JSON with `result_state = compute_result_state(...)`. `invalid=True` if any INVALID_EPOCH condition was recorded during the epoch (firewall breach, lockdown drift, model fallback, prompt_hash divergence).

- [ ] **Step 4: Run to verify it passes** — result-state test PASSES.

- [ ] **Step 5: Commit**
```bash
git add scripts/kalshi_advise/competition_report.py tests/test_kalshi_competition.py
git commit -m "feat(kalshi): competition report with preregistered mechanical result states"
```

---

## Self-Review (completed)

- **Spec coverage:** §1 pinning → T3 (+T2 effort); §3.1 atomic paired-open → T5; §4 firewall/lockdown → T3+T4; §5.1 byte-frozen prompt → T1+T2; §5.2 coverage/anti-abstention + preregistered rule → T6; §6 result states → T6; §7 data flow → T5+T6. No uncovered requirement.
- **Placeholder scan:** the only non-literal spots are the C++ mechanism in T5 (delegated to Codex against named existing files/patterns — an explicit, bounded instruction, not a vague TODO) and the report SQL in T6 (reuses the existing `advise score` path). All Python deliverables carry complete code.
- **Type consistency:** `prompt_hash`, `competition_pair_id`, `context_hash`, epoch ids, and `compute_result_state` signature are consistent across T1–T6.

## Execution Handoff

This plan is a **handoff to Codex** (the implementer). Codex should implement task-by-task, each ending in the stated test + commit, and open a PR gated on CI. Claude will review each task/PR the same flag→fix→verify way (firewall probes green, lockdown-drift abstains, pairing invariant holds, result-state math correct) before merge — and will NOT grade Claude's lane gently (§10 of spec: the result is mechanical anyway).
