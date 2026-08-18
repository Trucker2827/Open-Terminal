import json,os,sys,tempfile,unittest
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..",".."));sys.path.insert(0,os.path.join(ROOT,"scripts","research"))
import commodity_chronos_shadow as s
def report(family,ts):return {"generated_at_ms":ts,"by_family":{family:{"predictions":{family+"-26AUG1823-T100":{"p_yes_full":.8,"market_yes_ask":.6,"market_yes_bid":.59,"features":{"floor_strike":100,"spot":99}}}}}}
class TestCommodityChronos(unittest.TestCase):
 def test_hashes_and_authority_are_independent(self):
  hashes={s.policy_hash(f) for f in s.SPECS};self.assertEqual(len(hashes),3)
  for f in s.SPECS:self.assertEqual(s.policy(f)["authority"],"paper_research_only_no_order_api")
 def test_agreement_and_settlement(self):
  with tempfile.TemporaryDirectory() as d:
   path=os.path.join(d,"state.json");f="KXGOLDH";s.run_once(f,path,now_ms=1000,report={},chrono={},settlements=[])
   chrono={"journal_id":"c","created_at":1100,"direction":"up","predicted_return_bps":200,"reference_price":99}
   s.run_once(f,path,now_ms=1200,report=report(f,1100),chrono=chrono,settlements=[])
   out=s.run_once(f,path,now_ms=1300,report=report(f,1100),chrono=chrono,settlements=[{"ticker":f+"-26AUG1823-T100","result":"YES"}])
   self.assertEqual(out["summary"]["cohorts"]["agreement"]["completed"],1);self.assertGreater(out["summary"]["cohorts"]["control_alone"]["net_pnl"],0)
   with open(path,encoding="utf-8") as h:state=json.load(h)
   self.assertIn("postmortem",next(iter(state["records"].values())))
   self.assertGreaterEqual(out["summary"]["diagnostics"]["runs"],2)
 def test_pre_freeze_report_ignored_and_hash_tamper_fails(self):
  with tempfile.TemporaryDirectory() as d:
   path=os.path.join(d,"state.json");f="KXSILVERH";s.run_once(f,path,now_ms=5000,report={},chrono={},settlements=[]);s.run_once(f,path,now_ms=6000,report=report(f,4000),chrono={"journal_id":"c","created_at":4000,"predicted_return_bps":1,"reference_price":99},settlements=[])
   with open(path,encoding="utf-8") as handle:state=json.load(handle)
   self.assertFalse(state["records"]);state["policy_sha256"]="bad";s.atomic_write(path,state)
   with self.assertRaises(RuntimeError):s.load(path,f,7000)
if __name__=="__main__":unittest.main()
