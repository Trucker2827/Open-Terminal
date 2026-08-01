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
