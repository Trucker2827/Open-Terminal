import os,sys,unittest
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..",".."));sys.path.insert(0,os.path.join(ROOT,"scripts","research"))
import kxbtcd_hourly_regime_audit as a
class AuditTest(unittest.TestCase):
 def test_filters_15m_and_clusters_one_per_event(self):
  f={k:0.0 for k in a.FULL_FEATURES};f.update(yes_mid=.9,sqrt_minutes_left=6)
  rows=[{"ticker":"KXBTCD-E-T1","event_ticker":"KXBTCD-E","outcome":True,"observations":[f,f]}, {"ticker":"KXBTC15M-X","event_ticker":"KXBTC15M-X","outcome":False,"observations":[f]}]
  out=a.audit(rows,min_edge=0);self.assertEqual(out["early_30m_plus"]["events"],1);self.assertFalse(out["early_30m_plus"]["executable_pnl_proven"])
 def test_empty_does_not_pass(self):self.assertFalse(a.audit([])["late_under_10m"]["passes_forecast_gate"])
if __name__=="__main__":unittest.main()
