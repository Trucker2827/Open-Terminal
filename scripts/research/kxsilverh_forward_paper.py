#!/usr/bin/env python3
"""Hash-locked, paper-only KXSILVERH hourly value trial."""
from __future__ import annotations
import argparse,datetime as dt,fcntl,hashlib,json,math,os,tempfile,time
import kalshi_edge_common as common

FAMILY="KXSILVERH"; REPORT_STEM="commodities-hourly-calibrator"
POLICY={"family":FAMILY,"horizon":"hourly","minimum_executable_edge":.10,
 "max_entry_all_in_usd":2.0,"entry_price_min":.05,"entry_price_max":.95,
 "max_report_age_ms":20*60_000,"selection":"one highest-edge KXSILVERH strike per event",
 "exit":"hold to settlement","authority":"paper_research_only_no_order_api"}

def batch_fee(price,n):
 return 0.0 if n<=0 else math.ceil(.07*n*max(0,min(1,float(price)))*(1-max(0,min(1,float(price))))*100-1e-12)/100
def size_for_cap(price,cap=2.0):
 if not 0<price<1:return 0
 n=int(cap//price)
 while n and n*price+batch_fee(price,n)>cap+1e-9:n-=1
 return n
def policy_hash():return hashlib.sha256(json.dumps(POLICY,sort_keys=True,separators=(",",":")).encode()).hexdigest()
def initial_state(now):return {"schema":1,"event":"kxsilverh_forward_paper_state","policy":POLICY,"policy_sha256":policy_hash(),"frozen_at_ms":int(now),"records":{}}
def atomic_write(path,data):
 os.makedirs(os.path.dirname(os.path.abspath(path)),exist_ok=True);fd,tmp=tempfile.mkstemp(prefix=".kxsilverh-forward-",dir=os.path.dirname(os.path.abspath(path)),text=True)
 try:
  with os.fdopen(fd,"w") as f:json.dump(data,f,indent=2,sort_keys=True);f.write("\n");f.flush();os.fsync(f.fileno())
  os.replace(tmp,path)
 finally:
  if os.path.exists(tmp):os.unlink(tmp)
def load_state(path,now):
 if not os.path.exists(path):return initial_state(now),True
 with open(path) as f:s=json.load(f)
 if s.get("policy")!=POLICY or s.get("policy_sha256")!=policy_hash():raise RuntimeError("paper policy mismatch; refusing to tune existing trial")
 return s,False
def event_of(t):return t.rsplit("-T",1)[0]
def candidate(t,p):
 if not str(t).startswith(FAMILY+"-"):return None
 try:q=float(p["p_yes_full"])
 except (KeyError,TypeError,ValueError):return None
 choices=[]
 if p.get("market_yes_ask") is not None:choices.append((q-float(p["market_yes_ask"]),"YES",float(p["market_yes_ask"])))
 if p.get("market_yes_bid") is not None:choices.append((float(p["market_yes_bid"])-q,"NO",1-float(p["market_yes_bid"])))
 choices=[x for x in choices if POLICY["entry_price_min"]<=x[2]<=POLICY["entry_price_max"]]
 if not choices:return None
 edge,side,price=max(choices)
 if edge<POLICY["minimum_executable_edge"]:return None
 return {"ticker":t,"event_ticker":event_of(t),"side":side,"entry_price":price,"model_probability":q if side=="YES" else 1-q,"executable_edge":edge}
def best_by_event(preds):
 out={}
 for t,p in preds.items():
  c=candidate(t,p)
  if c and (c["event_ticker"] not in out or c["executable_edge"]>out[c["event_ticker"]]["executable_edge"]):out[c["event_ticker"]]=c
 return list(out.values())
def _yes(v):
 if isinstance(v,bool):return v
 if isinstance(v,str):return v.strip().upper()=="YES"
 return None
def outcomes_from_hourly_state(state=None,settlements=None):
 out={}
 if state is None:
  p=common.evidence_path(REPORT_STEM+"-state.json");state=json.load(open(p)) if os.path.exists(p) else {}
 for r in ((state.get("by_family") or {}).get(FAMILY) or {}).get("resolved_record") or []:
  if r.get("ticker") and r.get("outcome") is not None:out[str(r["ticker"])]=bool(r["outcome"])
 if settlements is None:settlements,_=common.read_jsonl("kalshi-settlements.jsonl")
 for r in settlements:
  t=str(r.get("kalshi_market_id") or r.get("ticker") or "")
  if t.startswith(FAMILY+"-"):
   y=_yes(r.get("result") or r.get("market_result"))
   if y is not None:out[t]=y
 return out
def settle(s,outcomes,now):
 for r in s["records"].values():
  if r["status"]!="open" or r["ticker"] not in outcomes:continue
  won=outcomes[r["ticker"]]==(r["side"]=="YES");r.update(status="completed",outcome_yes=outcomes[r["ticker"]],won=won,settled_at_ms=now,net_pnl=(r["contracts"] if won else 0)-r["entry_all_in"])
def summary(s):
 done=sorted((r for r in s["records"].values() if r["status"]=="completed"),key=lambda r:r.get("settled_at_ms",0));eq=peak=dd=0
 for r in done:eq+=r["net_pnl"];peak=max(peak,eq);dd=max(dd,peak-eq)
 return {"frozen_at_ms":s["frozen_at_ms"],"policy_sha256":s["policy_sha256"],"family":FAMILY,"completed":len(done),"open":sum(r["status"]=="open" for r in s["records"].values()),"net_pnl":sum(r["net_pnl"] for r in done),"win_rate":sum(r["won"] for r in done)/len(done) if done else None,"max_drawdown":dd,"validation_target":100,"remaining":max(0,100-len(done)),"authority":POLICY["authority"]}
def apply_report(s,report,now):
 gen=int(report.get("generated_at_ms") or 0)
 if gen<s["frozen_at_ms"] or now-gen>POLICY["max_report_age_ms"]:return
 preds=(((report.get("by_family") or {}).get(FAMILY) or {}).get("predictions") or {})
 for c in best_by_event(preds):
  if c["event_ticker"] in s["records"]:continue
  n=size_for_cap(c["entry_price"]);fee=batch_fee(c["entry_price"],n)
  if n:s["records"][c["event_ticker"]]={**c,"horizon":"hourly","family":FAMILY,"minimum_edge":.10,"status":"open","observed_at_ms":now,"entry_ts_ms":now,"report_generated_at_ms":gen,"contracts":n,"entry_fee":fee,"entry_all_in":n*c["entry_price"]+fee}
def run_once(path,now_ms=None,report=None,outcomes=None):
 now=int(time.time()*1000) if now_ms is None else int(now_ms);os.makedirs(os.path.dirname(os.path.abspath(path)),exist_ok=True);fd=os.open(os.path.abspath(path)+".lock",os.O_CREAT|os.O_RDWR,0o644)
 try:
  fcntl.flock(fd,fcntl.LOCK_EX);s,created=load_state(path,now)
  if not created:
   if report is None:
    with open(common.evidence_path(REPORT_STEM+".json")) as f:report=json.load(f)
   apply_report(s,report,now);settle(s,outcomes if outcomes is not None else outcomes_from_hourly_state(),now)
  s["updated_at"]=dt.datetime.fromtimestamp(now/1000,dt.timezone.utc).isoformat();atomic_write(path,s);return {"created":created,"state_path":os.path.abspath(path),"policy":POLICY,"summary":summary(s)}
 finally:fcntl.flock(fd,fcntl.LOCK_UN);os.close(fd)
def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("--state",default=common.evidence_path("kxsilverh-forward-paper.json"));p.add_argument("--json",action="store_true");a=p.parse_args(argv);print(json.dumps(run_once(a.state),indent=None if a.json else 2,sort_keys=True));return 0
if __name__=="__main__":raise SystemExit(main())
