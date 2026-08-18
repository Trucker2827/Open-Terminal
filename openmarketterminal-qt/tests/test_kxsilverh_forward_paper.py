import json,os,sys,tempfile,unittest
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..",".."));sys.path.insert(0,os.path.join(ROOT,"scripts","research"))
import kxsilverh_forward_paper as t
class SilverTest(unittest.TestCase):
 def test_policy_and_family_isolation(self):
  self.assertEqual(t.FAMILY,"KXSILVERH");self.assertEqual(t.POLICY["minimum_executable_edge"],.10);self.assertEqual(t.POLICY["authority"],"paper_research_only_no_order_api")
  self.assertIsNone(t.candidate("KXGOLDH-26AUG1717-T1",{"p_yes_full":.8,"market_yes_ask":.6}))
 def test_clean_freeze_best_strike_settlement_and_hash(self):
  with tempfile.TemporaryDirectory() as d:
   p=os.path.join(d,"state.json");t.run_once(p,1000,report={},outcomes={})
   report={"generated_at_ms":1100,"by_family":{"KXSILVERH":{"predictions":{"KXSILVERH-26AUG1717-T1":{"p_yes_full":.75,"market_yes_ask":.6,"market_yes_bid":.59},"KXSILVERH-26AUG1717-T2":{"p_yes_full":.85,"market_yes_ask":.6,"market_yes_bid":.59}}}}}
   t.run_once(p,1200,report=report,outcomes={});out=t.run_once(p,1300,report=report,outcomes={"KXSILVERH-26AUG1717-T2":True})
   self.assertEqual(out["summary"]["completed"],1);self.assertGreater(out["summary"]["net_pnl"],0)
   with open(p) as f:s=json.load(f)
   self.assertEqual(next(iter(s["records"].values()))["ticker"],"KXSILVERH-26AUG1717-T2")
 def test_venue_settlement_filter(self):
  rows=[{"kalshi_market_id":"KXSILVERH-26AUG1717-T1","result":"no"},{"kalshi_market_id":"KXGOLDH-26AUG1717-T1","result":"yes"}]
  self.assertEqual(t.outcomes_from_hourly_state(state={},settlements=rows),{"KXSILVERH-26AUG1717-T1":False})
if __name__=="__main__":unittest.main()
