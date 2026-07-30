import os, sys, json, unittest, tempfile, datetime
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "research"))
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

class SimTest(unittest.TestCase):
    def _path(self):
        # bet-side (ts_ms, bid, ask); price rises 0.20 -> 0.45 then settles
        return [(1000, 0.19, 0.20), (2000, 0.30, 0.31), (3000, 0.44, 0.45)]

    def test_take_profit_sells_at_bid_net_of_fees(self):
        path = self._path()
        # entered at ask 0.20 at i0=0; TP +0.20 -> first bid >= 0.40 is 0.44 @ t=3000
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "tp", "amount": 0.20}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.44 - 0.20) - sg.common.fee_per_contract(0.20) - sg.common.fee_per_contract(0.44)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_hold_uses_settlement_not_a_sale(self):
        path = self._path()
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "hold", "amount": None}, won=True)
        self.assertFalse(r["exited"])
        self.assertAlmostEqual(r["pnl"], (1.0 - 0.20) - sg.common.fee_per_contract(0.20), places=6)

    def test_stop_loss_triggers_on_decline(self):
        path = [(1000, 0.49, 0.50), (2000, 0.33, 0.34), (3000, 0.20, 0.21)]
        # entry ask 0.50; SL -0.15 -> first bid <= 0.35 is 0.33 @ t=2000
        r = sg.contract_pnl(path, 0, 0.50, {"kind": "sl", "amount": 0.15}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.33 - 0.50) - sg.common.fee_per_contract(0.50) - sg.common.fee_per_contract(0.33)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_bet_side_quotes_flips_for_NO(self):
        yes = [(1000, 0.60, 0.62)]
        self.assertEqual(sg.bet_side_quotes(yes, "NO"), [(1000, 1 - 0.62, 1 - 0.60)])

    def test_trailing_stop_exits_on_pullback_from_peak(self):
        # entry ask 0.30; bids rise to a 0.50 peak then pull back to 0.39;
        # trail -0.10 -> 0.39 <= 0.50-0.10 -> exit at 0.39 (taker at bid).
        path = [(1000, 0.34, 0.35), (2000, 0.50, 0.51), (3000, 0.39, 0.40)]
        r = sg.contract_pnl(path, 0, 0.30, {"kind": "trail", "amount": 0.10}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.39 - 0.30) - sg.common.fee_per_contract(0.30) - sg.common.fee_per_contract(0.39)
        self.assertAlmostEqual(r["pnl"], expected, places=6)
        # shallow pullback that never reaches the trail distance -> holds to settlement
        path2 = [(1000, 0.34, 0.35), (2000, 0.50, 0.51), (3000, 0.45, 0.46)]
        r2 = sg.contract_pnl(path2, 0, 0.30, {"kind": "trail", "amount": 0.10}, won=True)
        self.assertFalse(r2["exited"])
        self.assertAlmostEqual(r2["pnl"], (1.0 - 0.30) - sg.common.fee_per_contract(0.30), places=6)
