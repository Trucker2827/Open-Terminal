import os, sys, json, unittest, tempfile, datetime
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts", "research"))
import strategy_grid as sg  # noqa: E402

class GridTest(unittest.TestCase):
    def test_grid_is_pre_registered_and_ids_stable(self):
        g1 = sg.build_grid()
        g2 = sg.build_grid()
        ids = [v["variant_id"] for v in g1]
        self.assertEqual(len(ids), len(set(ids)))            # ids unique
        self.assertEqual([v["variant_id"] for v in g2], ids) # stable across calls
        # 2 sides * 6 bands * 3 gates * (1 hold + 5 tp + 4 sl + 3 trail) exits = 468
        self.assertEqual(len(g1), 2 * 6 * 3 * (1 + 5 + 4 + 3))
        v = g1[0]
        self.assertIn(v["side"], ("YES", "NO"))
        self.assertIn(v["gate"], ("mechanical", "physics", "calibrator"))

    def test_variant_id_encodes_the_tuple(self):
        v = {"side": "NO", "band": [0.10, 0.25], "gate": "physics",
             "exit": {"kind": "sl", "amount": 0.15}}
        vid = sg.variant_id(v)
        self.assertEqual(vid, "NO|b10-25|physics|sl15")
