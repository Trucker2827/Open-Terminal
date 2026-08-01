# Kalshi event-impact news signal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An LLM scores significant BTC events into `{direction, magnitude, half_life_hours}`; the Kalshi calibrator consumes a time-decaying `event_pressure` feature that must earn its weight through the existing ablation / beats-market gate.

**Architecture:** C++ producer → JSON evidence file → Python consumer, mirroring the existing btc-intelligence pipeline. A new hourly managed job runs a thin C++ command (`news bitcoin-event-impact`) that reads the pulse stories, pipes them to a Python LLM scorer, and writes `btc-event-impact.jsonl`/`-latest.json` via the existing snapshot helper. The calibrator reads that file as-of each decision and decays each live event to the exact decision time.

**Tech Stack:** Python 3 (stdlib only for the consumer; `anthropic` SDK for the scorer, imported lazily), C++/Qt (QProcess), unittest + ctest, `--selftest-*` headless harness.

## Global Constraints

- **The event-impact record schema (the shared interface between all three tasks) is EXACTLY:**
  ```json
  {
    "as_of_ms": "<epoch-ms as a STRING>",
    "events": [
      { "event_ts_ms": <int epoch ms>, "direction": <float -1..1>,
        "magnitude": <float 0..1>, "half_life_hours": <float > 0>,
        "kind": "<string>", "headline": "<string>", "rationale": "<string>" }
    ],
    "model": "<string>", "prompt_version": "<string>"
  }
  ```
  `as_of_ms` is a STRING (matches the btc-intelligence/pulse convention; the consumer parses it with `int(...)`). `events` may be empty (abstention / no live events). Nothing else is required reading for the consumer.
- **No-look-ahead is absolute.** Producer scores only stories with `published_ts_ms <= as_of_ms`. Consumer ignores any record with `as_of_ms > now_ms` and any event with `event_ts_ms > now_ms`; decay uses only elapsed wall-time. A leak fabricates a fake edge that loses real money.
- **Never lower the beats-market gate.** `event_pressure` is an ordinary ensemble feature; it enters the trusted forecast only by improving out-of-sample Brier (the unchanged `adds_value_over_market` / `MIN_SCORED_CONTRACTS = 100` gate). A useless signal → forecast stays untrusted → bot idle → nothing lost.
- **Neutral-on-missing, never a fabricated extreme.** Missing/stale/malformed/empty file → `event_pressure = 0.0`. Any producer failure → no new record → the bot behaves exactly as today.
- **No API key is ever read or logged.** The scorer resolves auth via the Anthropic SDK only (`claude_forecaster.py` pattern). No credential appears in code, args, or output.
- **The calibrator never calls the LLM.** The LLM runs only in the hourly producer, off the decision path.
- **Report schema stays additive.** `p_yes_full`, `market_yes_mid`, `adds_value_over_market`, per-contract predictions unchanged; the C++ bot/gate/cockpit need no change.
- **Decay convention:** `contribution = direction * magnitude * 0.5 ** (Δt_hours / half_life_hours)`, `Δt_hours = (now_ms - event_ts_ms) / 3_600_000`. Sum over live events, clamp to `[-1.0, 1.0]`.
- **Frozen scorer identity:** `PROMPT_VERSION = "event-impact-v1"`, `MODEL` default `claude-opus-4-8` (env-overridable). Editing the prompt text bumps the version.

## File Structure

- `openmarketterminal-qt/scripts/kalshi_advise/btc_event_impact.py` (new) — the LLM scorer. `identify` / `score` CLI, stdin→stdout, LLM-stubbable. One responsibility: turn pulse stories into the event-impact record.
- `openmarketterminal-qt/tests/test_btc_event_impact.py` (new) — scorer unit tests (LLM stubbed, no network).
- `openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py` (modify) — add the reader, the feature, the feature-list entry, and the observe_cycle wiring.
- `openmarketterminal-qt/tests/test_spot_calibrator.py` (modify) — consumer tests.
- `openmarketterminal-qt/tests/CMakeLists.txt` (modify) — register `test_btc_event_impact`.
- `openmarketterminal-qt/src/cli/CommandDispatch.cpp` (modify) — the `news bitcoin-event-impact` command + a stdin-capable process helper.
- `openmarketterminal-qt/src/cli/ServeCommand.cpp` (modify) — register the `btc-event-impact-hourly` managed job.
- `openmarketterminal-qt/src/app/main.cpp` (modify) — `--selftest-btc-event-impact`.
- `.github/workflows/regression.yml` (modify) — add `btc-event-impact` to the selftest loop.

---

### Task 1: The Python LLM scorer

**Files:**
- Create: `openmarketterminal-qt/scripts/kalshi_advise/btc_event_impact.py`
- Test: `openmarketterminal-qt/tests/test_btc_event_impact.py`
- Modify: `openmarketterminal-qt/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: stdin JSON `{ "as_of_ms": <int epoch ms>, "stories": [ {"headline": <str>, "published_ts_ms": <int epoch ms>}, ... ] }` (built by Task 3's C++ command).
- Produces: stdout JSON exactly the Global-Constraints record schema. `identify` → `{"provider","model","prompt_version"}`. The `score` function must be callable with an injected `scorer` callable so tests never hit the network.

- [ ] **Step 1: Write the failing tests**

```python
# openmarketterminal-qt/tests/test_btc_event_impact.py
import json, os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
import btc_event_impact as sc

AS_OF = 1_800_000_000_000

class IdentifyTest(unittest.TestCase):
    def test_identify_is_frozen_and_needs_no_sdk(self):
        ident = sc.identify()
        self.assertEqual(ident["provider"], "anthropic")
        self.assertEqual(ident["prompt_version"], "event-impact-v1")
        self.assertIn("model", ident)

class ScoreTest(unittest.TestCase):
    def _ctx(self, stories):
        return {"as_of_ms": AS_OF, "stories": stories}

    def test_score_emits_schema_and_echoes_as_of(self):
        # Injected model: one bearish hack event.
        def fake(system, user):
            return {"events": [{"event_ts_ms": AS_OF - 3_600_000, "direction": -0.8,
                                "magnitude": 0.7, "half_life_hours": 4.0,
                                "kind": "exchange-hack", "headline": "Exchange hacked",
                                "rationale": "theft is bearish"}]}
        out = sc.score(self._ctx([{"headline": "Exchange hacked",
                                   "published_ts_ms": AS_OF - 3_600_000}]), model_call=fake)
        self.assertEqual(out["as_of_ms"], str(AS_OF))
        self.assertEqual(out["prompt_version"], "event-impact-v1")
        self.assertEqual(len(out["events"]), 1)
        ev = out["events"][0]
        for k in ("event_ts_ms", "direction", "magnitude", "half_life_hours", "kind", "headline", "rationale"):
            self.assertIn(k, ev)
        self.assertLessEqual(ev["event_ts_ms"], AS_OF)  # no look-ahead

    def test_future_event_ts_is_dropped(self):
        def fake(system, user):
            return {"events": [{"event_ts_ms": AS_OF + 60_000, "direction": -0.5,
                                "magnitude": 0.5, "half_life_hours": 2.0,
                                "kind": "x", "headline": "future", "rationale": "r"}]}
        out = sc.score(self._ctx([]), model_call=fake)
        self.assertEqual(out["events"], [])  # event after as_of dropped

    def test_abstains_to_empty_on_noise(self):
        def fake(system, user):
            return {"events": []}
        out = sc.score(self._ctx([{"headline": "price ticks up", "published_ts_ms": AS_OF - 1000}]), model_call=fake)
        self.assertEqual(out["events"], [])

    def test_malformed_model_output_is_empty_not_crash(self):
        def fake(system, user):
            return {"nonsense": True}
        out = sc.score(self._ctx([]), model_call=fake)
        self.assertEqual(out["events"], [])
        self.assertEqual(out["as_of_ms"], str(AS_OF))

    def test_field_ranges_are_clamped(self):
        def fake(system, user):
            return {"events": [{"event_ts_ms": AS_OF - 1000, "direction": -5.0, "magnitude": 9.0,
                                "half_life_hours": -1.0, "kind": "k", "headline": "h", "rationale": "r"}]}
        out = sc.score(self._ctx([]), model_call=fake)
        # half_life <= 0 is invalid -> event dropped (no honest decay possible)
        self.assertEqual(out["events"], [])

    def test_valid_clamps_direction_and_magnitude(self):
        def fake(system, user):
            return {"events": [{"event_ts_ms": AS_OF - 1000, "direction": -5.0, "magnitude": 9.0,
                                "half_life_hours": 3.0, "kind": "k", "headline": "h", "rationale": "r"}]}
        ev = sc.score(self._ctx([]), model_call=fake)["events"][0]
        self.assertEqual(ev["direction"], -1.0)
        self.assertEqual(ev["magnitude"], 1.0)

class CliTest(unittest.TestCase):
    def test_main_score_reads_stdin_writes_stdout(self):
        # end-to-end through main() with the LLM monkeypatched off.
        sc._MODEL_CALL_FOR_TEST = lambda system, user: {"events": []}
        try:
            import io
            stdin = io.StringIO(json.dumps({"as_of_ms": AS_OF, "stories": []}))
            out = io.StringIO()
            rc = sc.main(["score"], stdin=stdin, stdout=out)
            self.assertEqual(rc, 0)
            parsed = json.loads(out.getvalue())
            self.assertEqual(parsed["events"], [])
        finally:
            sc._MODEL_CALL_FOR_TEST = None

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd openmarketterminal-qt && python tests/test_btc_event_impact.py -v`
Expected: FAIL (module `btc_event_impact` not found).

- [ ] **Step 3: Write the scorer**

Mirror `claude_forecaster.py` exactly for identity/auth/SDK-laziness. Key shape:

```python
#!/usr/bin/env python3
"""LLM event-impact scorer: pulse stories -> {direction, magnitude, half_life}
per significant BTC event. Mirrors claude_forecaster.py's frozen-identity,
lazy-SDK, no-key-logged contract. Pure stdin->stdout; the model call is
injectable so tests never touch the network."""
import json, os, sys

MODEL = os.environ.get("ANTHROPIC_MODEL", "claude-opus-4-8")
PROMPT_VERSION = "event-impact-v1"     # edit the prompt => bump this
EFFORT = os.environ.get("BTC_EVENT_IMPACT_EFFORT", "low")
_MODEL_CALL_FOR_TEST = None            # test seam; None in production

SYSTEM_PROMPT = (
    "You are a disciplined market-impact analyst for Bitcoin. Given recent news "
    "stories, identify ONLY events likely to move BTC's price materially over the "
    "next minutes-to-hours, and score each as a signed, decaying impulse. Abstain "
    "on routine or ambiguous news (return no event for it). A hack, exploit, or "
    "large theft from an exchange or bridge is bearish with a multi-hour half-life; "
    "an ETF inflow or favorable ruling is bullish. direction in [-1,1] "
    "(-1 strongly bearish, +1 strongly bullish); magnitude in [0,1] is expected "
    "price impact, NOT headline drama; half_life_hours > 0 is how fast it fades. "
    "event_ts_ms is the story's own publish time. Do not invent events not present "
    "in the stories."
)

SCHEMA = {  # json_schema for the model's structured output (events array)
    "type": "object",
    "properties": {"events": {"type": "array", "items": {
        "type": "object",
        "properties": {
            "event_ts_ms": {"type": "integer"},
            "direction": {"type": "number"}, "magnitude": {"type": "number"},
            "half_life_hours": {"type": "number"},
            "kind": {"type": "string"}, "headline": {"type": "string"},
            "rationale": {"type": "string"},
        },
        "required": ["event_ts_ms", "direction", "magnitude", "half_life_hours", "kind", "headline", "rationale"],
        "additionalProperties": False,
    }}},
    "required": ["events"],
    "additionalProperties": False,
}

def identify() -> dict:
    return {"provider": "anthropic", "model": MODEL, "prompt_version": PROMPT_VERSION}

def _clamp(x, lo, hi, default=None):
    try:
        x = float(x)
    except (TypeError, ValueError):
        return default
    return lo if x < lo else hi if x > hi else x

def _live_model_call(system, user):
    import anthropic  # lazy: identify() works without the SDK
    client = anthropic.Anthropic()
    resp = client.messages.create(
        model=MODEL, max_tokens=2048, system=system,
        messages=[{"role": "user", "content": user}],
        output_config={"format": {"type": "json_schema", "schema": SCHEMA}, "effort": EFFORT})
    if resp.stop_reason == "refusal":
        raise SystemExit("event-impact: model refused")
    text = next((b.text for b in resp.content if b.type == "text"), None)
    if not text:
        raise SystemExit("event-impact: empty response")
    return json.loads(text)

def score(ctx: dict, model_call=None) -> dict:
    as_of_ms = int(ctx.get("as_of_ms"))
    stories = ctx.get("stories") or []
    call = model_call or _MODEL_CALL_FOR_TEST or _live_model_call
    user = ("Stories (JSON). Score only materially price-moving BTC events; abstain otherwise.\n\n"
            + json.dumps({"as_of_ms": as_of_ms, "stories": stories}, sort_keys=True, indent=2))
    raw = call(SYSTEM_PROMPT, user) or {}
    events = []
    for e in (raw.get("events") or []):
        try:
            ts = int(e["event_ts_ms"])
        except (KeyError, TypeError, ValueError):
            continue
        if ts > as_of_ms:                      # no look-ahead
            continue
        hl = _clamp(e.get("half_life_hours"), 1e-6, 1e9)
        if hl is None or hl <= 0.0:            # no honest decay without a positive half-life
            continue
        d = _clamp(e.get("direction"), -1.0, 1.0)
        m = _clamp(e.get("magnitude"), 0.0, 1.0)
        if d is None or m is None:
            continue
        events.append({"event_ts_ms": ts, "direction": d, "magnitude": m,
                       "half_life_hours": hl, "kind": str(e.get("kind", ""))[:60],
                       "headline": str(e.get("headline", ""))[:300],
                       "rationale": str(e.get("rationale", ""))[:500]})
    return {"as_of_ms": str(as_of_ms), "events": events,
            "model": MODEL, "prompt_version": PROMPT_VERSION}

def main(argv=None, stdin=None, stdout=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    stdin = stdin or sys.stdin
    stdout = stdout or sys.stdout
    mode = argv[0] if argv else "score"
    if mode == "identify":
        stdout.write(json.dumps(identify()) + "\n"); return 0
    if mode == "score":
        raw = stdin.read()
        try:
            ctx = json.loads(raw) if raw.strip() else {"as_of_ms": 0, "stories": []}
        except json.JSONDecodeError as exc:
            sys.stderr.write(json.dumps({"error": f"bad context json: {exc}"}) + "\n"); return 2
        stdout.write(json.dumps(score(ctx)) + "\n"); return 0
    sys.stderr.write(f"usage: {sys.argv[0]} identify|score\n"); return 2

if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd openmarketterminal-qt && python tests/test_btc_event_impact.py -v`
Expected: PASS (all).

- [ ] **Step 5: Register the test in ctest**

In `openmarketterminal-qt/tests/CMakeLists.txt`, after the `test_spot_calibrator` block, add:

```cmake
add_test(NAME test_btc_event_impact
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_btc_event_impact.py -v)
```

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_advise/btc_event_impact.py \
        openmarketterminal-qt/tests/test_btc_event_impact.py \
        openmarketterminal-qt/tests/CMakeLists.txt
git commit -m "feat(kalshi): LLM event-impact scorer (direction/magnitude/half-life)"
```

---

### Task 2: The consumer `event_pressure` feature

**Files:**
- Modify: `openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py`
- Test: `openmarketterminal-qt/tests/test_spot_calibrator.py`

**Interfaces:**
- Consumes: `btc-event-impact-latest.json` in the Global-Constraints schema. Independent of Task 1's code (neutral-on-missing means it works before any producer exists).
- Produces: a new `ENSEMBLE_FEATURES` member `event_pressure`; the reader `load_event_impact_latest`; the pure feature `event_pressure_feature(record, now_ms)`. `reconcile_full_model` is unchanged (already generic); `FULL_FEATURES` grows 10→11.

- [ ] **Step 1: Write the failing tests** (append to `test_spot_calibrator.py`; `import spot_calibrator as cal`, `math` already imported there)

```python
class EventPressureTest(unittest.TestCase):
    NOW = 1_800_000_000_000
    def _rec(self, events, as_of=None):
        return {"as_of_ms": str(self.NOW if as_of is None else as_of), "events": events}

    def test_decay_at_one_half_life_is_half(self):
        hl = 4.0
        ev = {"event_ts_ms": self.NOW - int(hl * 3_600_000), "direction": 1.0,
              "magnitude": 1.0, "half_life_hours": hl, "kind": "k", "headline": "h", "rationale": "r"}
        val = cal.event_pressure_feature(self._rec([ev]), self.NOW)
        self.assertAlmostEqual(val, 0.5, places=6)

    def test_decay_at_zero_dt_is_full(self):
        ev = {"event_ts_ms": self.NOW, "direction": -1.0, "magnitude": 1.0,
              "half_life_hours": 3.0, "kind": "k", "headline": "h", "rationale": "r"}
        self.assertAlmostEqual(cal.event_pressure_feature(self._rec([ev]), self.NOW), -1.0, places=6)

    def test_events_sum_and_clamp(self):
        e = lambda d: {"event_ts_ms": self.NOW, "direction": d, "magnitude": 1.0,
                       "half_life_hours": 5.0, "kind": "k", "headline": "h", "rationale": "r"}
        # two strong bullish events sum > 1 then clamp to 1.0
        self.assertAlmostEqual(cal.event_pressure_feature(self._rec([e(0.8), e(0.8)]), self.NOW), 1.0, places=6)
        # opposing events partially cancel
        self.assertAlmostEqual(cal.event_pressure_feature(self._rec([e(0.6), e(-0.6)]), self.NOW), 0.0, places=6)

    def test_future_event_ignored(self):
        ev = {"event_ts_ms": self.NOW + 60_000, "direction": 1.0, "magnitude": 1.0,
              "half_life_hours": 3.0, "kind": "k", "headline": "h", "rationale": "r"}
        self.assertEqual(cal.event_pressure_feature(self._rec([ev]), self.NOW), 0.0)

    def test_record_from_future_is_ignored_by_loader(self):
        import tempfile, json as _j, os as _o
        fd, path = tempfile.mkstemp(suffix=".json"); _o.close(fd)
        try:
            with open(path, "w") as fh:
                _j.dump(self._rec([], as_of=self.NOW + 10_000), fh)
            self.assertIsNone(cal.load_event_impact_latest(path=path, now_ms=self.NOW))
        finally:
            _o.remove(path)

    def test_neutral_on_missing_or_empty(self):
        self.assertEqual(cal.event_pressure_feature(None, self.NOW), 0.0)
        self.assertEqual(cal.event_pressure_feature(self._rec([]), self.NOW), 0.0)
        self.assertEqual(cal.event_pressure_feature({"events": "garbage"}, self.NOW), 0.0)

    def test_event_pressure_in_feature_lists(self):
        self.assertIn("event_pressure", cal.ENSEMBLE_FEATURES)
        self.assertIn("event_pressure", cal.FULL_FEATURES)
        self.assertEqual(len(cal.FULL_FEATURES), 11)

    def test_migration_10_to_11_preserves_weights_through_settle(self):
        # A saved 10-feature model reconciles to 11: the 10 weights + bias survive,
        # event_pressure zero-inits. (Mirror the existing 6->10 migration test.)
        old = cal.OnlineLogit(cal.PHYSICS_FEATURES + ("book_imbalance", "trade_flow", "spot_drift", "news_forecast"))
        for i in range(len(old.w)):
            old.w[i] = 0.3 + 0.01 * i
            old.g2[i] = 1.0 + i
        migrated = cal.reconcile_full_model(old.to_json())
        self.assertEqual(tuple(migrated.features), cal.FULL_FEATURES)
        self.assertAlmostEqual(migrated.w[-1], old.w[-1])  # bias preserved
        idx = cal.FULL_FEATURES.index("event_pressure")
        self.assertEqual(migrated.w[idx], 0.0)             # new feature zero-init
        self.assertEqual(migrated.g2[idx], 0.0)
        for f in ("signed_distance_bps", "news_forecast"):
            self.assertAlmostEqual(migrated.w[migrated.features.index(f)], old.w[old.features.index(f)])
```

- [ ] **Step 2: Run the new tests to verify they fail**

Run: `cd openmarketterminal-qt && python tests/test_spot_calibrator.py EventPressureTest -v`
Expected: FAIL (`event_pressure_feature`/`load_event_impact_latest` missing; `event_pressure` not in feature lists; `FULL_FEATURES` len 10).

- [ ] **Step 3: Implement the feature and wiring**

In `spot_calibrator.py`:

1. Add the path constant next to `INTEL_LATEST_PATH` (~line 63):
```python
EVENT_IMPACT_LATEST_PATH = evidence_file("btc-event-impact-latest.json")
```
2. Grow the feature list (~line 90):
```python
ENSEMBLE_FEATURES = ("book_imbalance", "trade_flow", "spot_drift", "news_forecast", "event_pressure")
```
3. In `extract_features`, next to the other neutral stubs (~line 157):
```python
result["event_pressure"] = 0.0  # overridden in observe_cycle
```
4. Add the pure feature (near `news_forecast_feature`, ~line 298):
```python
def event_pressure_feature(record, now_ms):
    """Signed, decaying pressure from live BTC events, as-of now_ms.

    Sum over events with event_ts_ms <= now_ms of
    direction*magnitude*0.5**(dt_hours/half_life_hours), clamped [-1,1].
    0.0 when record is None/empty/malformed or has no live event -- neutral,
    never a fabricated extreme. Pure: the disk read is in
    load_event_impact_latest, so this is unit-testable against an injected
    record.
    """
    if not record:
        return 0.0
    try:
        events = record.get("events")
    except AttributeError:
        return 0.0
    if not isinstance(events, list):
        return 0.0
    total = 0.0
    for e in events:
        try:
            ts = int(e["event_ts_ms"])
            hl = float(e["half_life_hours"])
            d = float(e["direction"])
            m = float(e["magnitude"])
        except (KeyError, TypeError, ValueError):
            continue
        if ts > now_ms or hl <= 0.0:
            continue
        dt_hours = (now_ms - ts) / 3_600_000.0
        total += d * m * (0.5 ** (dt_hours / hl))
    return max(-1.0, min(1.0, total))
```
5. Add the reader (near `load_intel_latest`, ~line 428):
```python
def load_event_impact_latest(path=EVENT_IMPACT_LATEST_PATH, now_ms=None):
    """Load btc-event-impact-latest.json, guarded against a stale future write
    (no look-ahead via its own as_of_ms) and any read failure. None on either
    -- event_pressure_feature(None) is 0.0. Mirrors load_intel_latest.
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            record = json.load(fh)
    except (OSError, ValueError):
        return None
    if now_ms is not None:
        try:
            as_of_ms = int(record.get("as_of_ms"))
        except (TypeError, ValueError):
            return None
        if as_of_ms > now_ms:
            return None
    return record
```
6. Register it in `load_auxiliary_sources` (~line 438):
```python
        "intel": load_intel_latest(now_ms=now_ms),
        "event_impact": load_event_impact_latest(now_ms=now_ms),
```
7. Wire the override in `observe_cycle`: resolve once before the loop (next to `news_forecast = ...`, ~line 775):
```python
    event_pressure = event_pressure_feature(aux.get("event_impact"), now_ms)
```
and inside the loop, next to the other overrides (~line 789):
```python
        features["event_pressure"] = event_pressure
```

(`reconcile_full_model` needs NO change — it already preserves matching weights by name and zero-inits new ones; the feature-list growth drives the 10→11 migration.)

- [ ] **Step 4: Run the calibrator suite to verify all pass**

Run: `cd openmarketterminal-qt && python tests/test_spot_calibrator.py -v`
Expected: PASS (all prior tests + the new `EventPressureTest`; the ablation now A/Bs `event_pressure` automatically — confirm the existing ablation tests still pass).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py \
        openmarketterminal-qt/tests/test_spot_calibrator.py
git commit -m "feat(calibrator): decaying event_pressure feature (as-of, neutral-on-missing)"
```

---

### Task 3: The C++ producer command, managed job, and selftest

**Files:**
- Modify: `openmarketterminal-qt/src/cli/CommandDispatch.cpp`
- Modify: `openmarketterminal-qt/src/cli/ServeCommand.cpp`
- Modify: `openmarketterminal-qt/src/app/main.cpp`
- Modify: `.github/workflows/regression.yml`

**Interfaces:**
- Consumes: `btc-news-pulse-latest.json` (`stories[]` with `headline` + a publish timestamp) via `bitcoin_evidence_read_object`; the scorer script via `cli_script_path("btc_event_impact.py")`.
- Produces: `btc-event-impact-latest.json`/`.jsonl` (Global-Constraints schema) via `bitcoin_evidence_write_snapshot("btc-event-impact", obj)`; a `--selftest-btc-event-impact` one-shot.

- [ ] **Step 1: Add a stdin-capable process helper** in `CommandDispatch.cpp` (next to `run_capture`, ~line 2094):

```cpp
static CommandCapture run_capture_stdin(const QString& program, const QStringList& args,
                                        const QByteArray& stdin_bytes, int timeout_ms) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    CommandCapture out;
    if (!p.waitForStarted(2000)) { out.error = p.errorString(); return out; }
    p.write(stdin_bytes);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeout_ms)) {
        p.kill(); p.waitForFinished(1000); out.error = QStringLiteral("timeout"); return out;
    }
    out.stdout_text = QString::fromUtf8(p.readAllStandardOutput());
    out.stderr_text = QString::fromUtf8(p.readAllStandardError());
    out.ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (!out.ok && out.error.isEmpty())
        out.error = out.stderr_text.trimmed().isEmpty()
            ? QStringLiteral("exit %1").arg(p.exitCode()) : out.stderr_text.trimmed();
    return out;
}
```

- [ ] **Step 2: Add the `bitcoin-event-impact` subcommand** inside `news_command`, as a new `if` block after the `bitcoin-intelligence` block (~line 3899, before the final fallthrough). It mirrors the intelligence block's read-pulse/write-snapshot shape:

```cpp
    if (sub == QStringLiteral("bitcoin-event-impact") || sub == QStringLiteral("btc-event-impact")) {
        const int limit = parse_limit(args, 60);
        if (limit < 1 || limit > 200 || !args.isEmpty()) {
            std::fprintf(stderr, "usage: news bitcoin-event-impact [--limit N]\n");
            return 2;
        }
        const QString directory = bitcoin_evidence_data_dir();
        const QJsonObject pulse = bitcoin_evidence_read_object(
            directory + QStringLiteral("/btc-news-pulse-latest.json"));
        if (pulse.isEmpty()) {
            std::fprintf(stderr, "no Bitcoin pulse available; run `news bitcoin-pulse --force` first\n");
            return 5;
        }
        const qint64 as_of_ms = QDateTime::currentMSecsSinceEpoch();
        // Build blind stdin: only stories with a publish time <= as_of_ms (no look-ahead).
        QJsonArray stories;
        int taken = 0;
        for (const auto& value : pulse.value(QStringLiteral("stories")).toArray()) {
            if (taken >= limit) break;
            const QJsonObject s = value.toObject();
            // published_ts source unit: verify against BtcNewsPulse (published_ts).
            // Normalize to epoch-ms here; skip rows with no usable time or a future time.
            qint64 ts_ms = s.value(QStringLiteral("published_ts")).toVariant().toLongLong();
            if (ts_ms <= 0) continue;
            if (ts_ms < 100000000000LL) ts_ms *= 1000;   // seconds -> ms if needed
            if (ts_ms > as_of_ms) continue;
            stories.append(QJsonObject{{"headline", s.value(QStringLiteral("headline")).toString()},
                                       {"published_ts_ms", ts_ms}});
            ++taken;
        }
        const QByteArray stdin_bytes = QJsonDocument(QJsonObject{
            {"as_of_ms", as_of_ms}, {"stories", stories}}).toJson(QJsonDocument::Compact);

        const QString py = cli_python_for_scripts();
        QString script = qEnvironmentVariable("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
        if (script.isEmpty()) script = cli_script_path(QStringLiteral("btc_event_impact.py"));
        if (py.isEmpty() || script.isEmpty()) {
            std::fprintf(stderr, "event-impact scorer unavailable (python/script missing)\n");
            return 5;
        }
        const CommandCapture r = run_capture_stdin(py, {script, QStringLiteral("score")}, stdin_bytes, 60000);
        if (!r.ok) {
            std::fprintf(stderr, "event-impact scorer failed: %s\n", qUtf8Printable(r.error));
            return 5;   // NO snapshot written on failure -- consumer keeps last good / neutral
        }
        const QJsonDocument doc = QJsonDocument::fromJson(r.stdout_text.toUtf8());
        if (!doc.isObject() || !doc.object().contains(QStringLiteral("events"))
                            || !doc.object().contains(QStringLiteral("as_of_ms"))) {
            std::fprintf(stderr, "event-impact scorer produced malformed output\n");
            return 5;   // never write a garbage snapshot
        }
        if (!bitcoin_evidence_write_snapshot(QStringLiteral("btc-event-impact"), doc.object())) {
            std::fprintf(stderr, "failed to write btc-event-impact snapshot\n");
            return 5;
        }
        if (opts.json) std::printf("%s\n", qUtf8Printable(r.stdout_text.trimmed()));
        else std::printf("btc-event-impact: %d event(s) scored\n",
                         doc.object().value(QStringLiteral("events")).toArray().size());
        return 0;
    }
```

(Note for the implementer: verify the pulse story's timestamp field name/unit against `src/services/news/BtcNewsPulse.{h,cpp}` — the struct field is `published_ts`; confirm whether the JSON key is `published_ts`/`published` and whether it is epoch-ms or -sec, and adjust the normalization accordingly. The `< 1e11` seconds→ms heuristic is a safety net, not a substitute for checking.)

- [ ] **Step 3: Register the managed job** in `ServeCommand.cpp`, in the job registry next to `btc-evidence-hourly-intelligence` (~line 6761):

```cpp
            {QStringLiteral("btc-event-impact-hourly"), QStringLiteral("Bitcoin hourly event-impact scorer"),
             QStringLiteral("Score materially price-moving BTC events (direction, magnitude, half-life) via the LLM scorer, off the decision path."),
             {QStringLiteral("news"), QStringLiteral("bitcoin-event-impact"),
              QStringLiteral("--limit"), QStringLiteral("60")},
             3600, 240},
```

(Interval 3600s; offset 240 so it runs after the pulse job (offset 180) has written fresh stories.)

- [ ] **Step 4: Add the selftest** `--selftest-btc-event-impact` in `main.cpp` (alongside the other `--selftest-*` dispatches, ~line 868). It must run offscreen with no network and prove the glue's two branches. Write a small self-contained routine (e.g. `BtcEventImpactSelftest.h` under `src/cli/` or inline) that:
  1. writes a fixture `btc-news-pulse-latest.json` into a temp `GenericDataLocation` (set `HOME`/`XDG_DATA_HOME` to a temp dir for the run, or point `bitcoin_evidence_data_dir` via the same env the tests use),
  2. sets `OPENTERMINAL_BTC_EVENT_IMPACT_SCORER` to a stub script that echoes a fixture record (a tiny python one-liner written to the temp dir), invokes `news bitcoin-event-impact`, and asserts `btc-event-impact-latest.json` exists and parses with an `events` array,
  3. re-points the env var at a stub that exits non-zero, invokes again, and asserts the command returns non-zero AND did not overwrite the good snapshot,
  4. prints `SELFTEST btc-event-impact OK` and returns 0, or a diagnostic and non-zero.

Keep it dependency-light and deterministic. (If wiring a temp `GenericDataLocation` is awkward, gate the data dir behind an existing test env override if one exists; otherwise use `QStandardPaths::setTestModeEnabled(true)` which redirects GenericDataLocation to a test path.)

- [ ] **Step 5: Add the selftest to CI** in `.github/workflows/regression.yml` (~line 124), appending to the loop list:

```yaml
          for t in tools datahub-peek feeds dock-layout universe-scan paper portfolio-replication bridge-discovery workflow-honesty btc-event-impact; do
```

- [ ] **Step 6: Build and run the selftest locally**

Run:
```bash
cd openmarketterminal-qt && cmake --build build --target OpenTerminal --parallel
QT_QPA_PLATFORM=offscreen ./build/OpenTerminal.app/Contents/MacOS/OpenTerminal --selftest-btc-event-impact
```
Expected: `SELFTEST btc-event-impact OK`, exit 0. (Use the repo's actual build dir/binary path.)

- [ ] **Step 7: Confirm the managed job registers** (no live LLM needed):

Run: `./build/.../OpenTerminal <the CLI entry> news bitcoin-event-impact --limit 5` against a machine with a pulse file, OR rely on the selftest. Confirm `sandbox install-jobs` (or the equivalent) lists `btc-event-impact-hourly`. Document the command actually run and its output.

- [ ] **Step 8: Commit**

```bash
git add openmarketterminal-qt/src/cli/CommandDispatch.cpp \
        openmarketterminal-qt/src/cli/ServeCommand.cpp \
        openmarketterminal-qt/src/app/main.cpp .github/workflows/regression.yml
git commit -m "feat(kalshi): hourly event-impact producer command + managed job + selftest"
```

---

## Self-Review notes (for the implementer/reviewer)

- **Spec coverage:** Task 1 = the LLM scorer (design §Producer/scorer); Task 2 = the decaying consumer feature + migration + ablation (design §Consumer, §Testing); Task 3 = the C++ producer command + hourly job + failure-isolation (design §Producer). No-look-ahead is enforced on both sides (scorer drops `event_ts > as_of`; consumer drops `as_of > now` and `event_ts > now`).
- **The gate is untouched:** `event_pressure` is just another `ENSEMBLE_FEATURE`; the ablation A/Bs it and the 100-contract beats-market floor is unchanged. Expect an honest empty verdict at first.
- **Deferred (not in this plan):** the sub-hourly breaking-event scan; hold/cut; the 15-minute brain.
- **Activation:** Task 3 needs a serve-daemon rebuild + redeploy for the new job to run hourly (same as prior C++ evidence changes) — separate from merging the PR.
