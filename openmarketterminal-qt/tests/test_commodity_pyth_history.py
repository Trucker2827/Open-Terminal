import json,os,sqlite3,sys,tempfile,unittest
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..",".."));sys.path.insert(0,os.path.join(ROOT,"scripts","research"))
import commodity_pyth_history as subject

class CommodityPythHistoryTest(unittest.TestCase):
 def test_independent_pyth_only_capture_and_missing_wti(self):
  with tempfile.TemporaryDirectory() as d:
   db=os.path.join(d,"db.sqlite");seed=os.path.join(d,"seed.json")
   branches={}
   for family,spec in subject.SPECS.items():branches[spec["race_family"]]={"pyth_series":{spec["pyth_symbol"]:[[1000,10.0]]}}
   with open(seed,"w",encoding="utf-8") as handle:json.dump({"by_family":branches},handle)
   latest={subject.SPECS["KXGOLDH"]["pyth_id"]:(11.0,.1,2000),subject.SPECS["KXSILVERH"]["pyth_id"]:(12.0,.1,2000)}
   out=subject.collect(db,seed,3000,latest)
   self.assertEqual(out["families"]["KXGOLDH"]["status"],"READY")
   self.assertEqual(out["families"]["KXWTIH"]["status"],"WAITING_FOR_SETTLEMENT_FEED")
   con=sqlite3.connect(db);rows=con.execute("select symbol,source from edge_prediction_raw_ticks").fetchall();con.close()
   self.assertTrue(rows);self.assertTrue(all(src.startswith("pyth:") for _,src in rows))
   self.assertFalse(any("yahoo" in src.lower() for _,src in rows))
 def test_idempotent(self):
  with tempfile.TemporaryDirectory() as d:
   db=os.path.join(d,"db.sqlite");seed=os.path.join(d,"missing.json");spec=subject.SPECS["KXGOLDH"]
   latest={spec["pyth_id"]:(11.0,.1,2000)}
   subject.collect(db,seed,3000,latest);subject.collect(db,seed,4000,latest)
   con=sqlite3.connect(db);self.assertEqual(con.execute("select count(*) from edge_prediction_raw_ticks").fetchone()[0],1);con.close()
if __name__=="__main__":unittest.main()
