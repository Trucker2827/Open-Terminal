#!/usr/bin/env python3
"""Read-only clustered walk-forward screen for KXBTCD hourly regimes."""
from __future__ import annotations
import argparse,json,os,sys
import kalshi_edge_common as common
QT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..","..","openmarketterminal-qt","scripts","kalshi_advise"));sys.path.insert(0,QT)
from spot_calibrator import FULL_FEATURES,L2,OnlineLogit

WINDOWS={"early_30m_plus":lambda m:m>=30,"middle_10_to_30m":lambda m:10<=m<30,"late_under_10m":lambda m:m<10}

def event_rows(records):
 out={}
 for r in records:
  if not str(r.get("ticker","")).startswith("KXBTCD-"):continue
  out.setdefault(r.get("event_ticker") or r["ticker"].rsplit("-T",1)[0],[]).append(r)
 return sorted(out.items())

def audit(records,min_edge=.10):
 model=OnlineLogit(FULL_FEATURES); samples={k:[] for k in WINDOWS}
 for event,contracts in event_rows(records):
  candidates={k:[] for k in WINDOWS}
  for r in contracts:
   outcome=bool(r.get("outcome"))
   for f in r.get("observations") or []:
    minutes=float(f.get("sqrt_minutes_left",0))**2;p=model.predict(f);mid=float(f.get("yes_mid",.5));edge=abs(p-mid)
    for name,inside in WINDOWS.items():
     if inside(minutes):candidates[name].append((edge,p,mid,outcome,r["ticker"],minutes))
  for name,rows in candidates.items():
   if rows:
    edge,p,mid,y,ticker,minutes=max(rows)
    if edge>=min_edge:
     side_yes=p>mid;price=mid if side_yes else 1-mid;won=y==side_yes
     samples[name].append({"event":event,"ticker":ticker,"edge":edge,"model_brier":(p-y)**2,"market_brier":(mid-y)**2,"upper_bound_pnl":(1-price if won else -price),"minutes_left":minutes})
  # Entire ladder trains only after every prediction for its event.
  for r in contracts:
   for f in r.get("observations") or []:model.update(f,bool(r.get("outcome")),l2=L2)
 return {name:summarize(rows) for name,rows in samples.items()}

def summarize(rows):
 blocks=[];n=len(rows)
 for i in range(4):
  part=rows[i*n//4:(i+1)*n//4];imp=sum(x["market_brier"]-x["model_brier"] for x in part)/len(part) if part else None
  blocks.append({"n":len(part),"brier_improvement":imp,"upper_bound_pnl":sum(x["upper_bound_pnl"] for x in part)})
 return {"events":n,"blocks":blocks,"passes_forecast_gate":bool(n>=40 and all(b["n"]>=10 and b["brier_improvement"]>0 for b in blocks)),"executable_pnl_proven":False,"note":"Mid-price P&L excludes spread, fees and fillability; it is an optimistic screen only."}

def run(state=None):
 if state is None:
  with open(common.evidence_path("spot-calibrator-state.json")) as f:state=json.load(f)
 return {"event":"kxbtcd_hourly_regime_audit","read_only":True,"family":"KXBTCD","minimum_model_mid_gap":.10,"regimes":audit(state.get("resolved_record") or []),"paper_trial_authorized":False}
def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("--json",action="store_true");a=p.parse_args(argv);print(json.dumps(run(),indent=None if a.json else 2,sort_keys=True));return 0
if __name__=="__main__":raise SystemExit(main())
