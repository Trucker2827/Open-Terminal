import json
import os
import sys
import tempfile
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "ai_quant_lab"))
sys.path.insert(0, SCRIPTS)
import signal_publisher as sp


IC_GOOD = {"Rank_IC_mean": 0.02, "Rank_ICIR": 0.5, "IC_mean": 0.015,
           "days": 500, "positive_ic_days": 0.55}   # tstat = 0.5*sqrt(500) ~ 11.2


class PayloadTest(unittest.TestCase):
    def test_trusted_requires_positive_ic_and_sample(self):
        ranked = [{"symbol": "btc", "score": 0.4}, {"symbol": "eth", "score": -0.2}]
        p = sp.build_signal_payload("m1", "2026-07-22", ranked, IC_GOOD, 123)
        self.assertTrue(p["trusted"])
        self.assertTrue(p["advisory_only"])
        self.assertEqual(p["signals"]["BTC"], {"score": 0.4, "rank": 1, "direction": "up"})
        self.assertEqual(p["signals"]["ETH"]["direction"], "down")

    def test_untrusted_without_ic(self):
        p = sp.build_signal_payload("m1", "2026-07-22", [{"symbol": "BTC", "score": 1.0}],
                                    None, 0)
        self.assertFalse(p["trusted"])
        self.assertIsNone(p["trailing_ic"])

    def test_untrusted_with_weak_thin_or_insignificant_ic(self):
        weak = dict(IC_GOOD, Rank_IC_mean=0.001)
        thin = dict(IC_GOOD, days=50)
        # The live crypto model's actual numbers: positive mean, t ~ 0.5 —
        # statistically nothing, and precisely what trust must reject.
        insignificant = {"Rank_IC_mean": 0.0177, "Rank_ICIR": 0.0225,
                         "IC_mean": 0.009, "days": 520, "positive_ic_days": 0.496}
        for ic in (weak, thin, insignificant):
            p = sp.build_signal_payload("m1", "x", [{"symbol": "BTC", "score": 1.0}], ic, 0)
            self.assertFalse(p["trusted"], ic)

    def test_zero_score_is_flat(self):
        p = sp.build_signal_payload("m1", "x", [{"symbol": "BTC", "score": 0.0}], None, 0)
        self.assertEqual(p["signals"]["BTC"]["direction"], "flat")

    def test_atomic_save_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.environ["OPENTERMINAL_EVIDENCE_DIR"] = tmp
            try:
                payload = sp.build_signal_payload("m1", "x", [], IC_GOOD, 7)
                sp.save_atomic(payload, sp.evidence_path())
                with open(os.path.join(tmp, "quant-signals.json")) as fh:
                    loaded = json.load(fh)
                self.assertEqual(loaded["generated_at_ms"], 7)
                self.assertFalse(os.path.exists(sp.evidence_path() + ".tmp"))
            finally:
                os.environ.pop("OPENTERMINAL_EVIDENCE_DIR", None)


if __name__ == "__main__":
    unittest.main()
