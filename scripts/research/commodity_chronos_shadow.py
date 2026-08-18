#!/usr/bin/env python3
"""Independent hash-locked Chronos shadow ledger for one hourly commodity family."""
from __future__ import annotations
import argparse,datetime as dt,fcntl,hashlib,json,math,os,sqlite3,tempfile,time
import kalshi_edge_common as common

SPECS={
 "KXGOLDH":{"symbol":"XAU-USD","raw_symbol":"XAU"},
 "KXSILVERH":{"symbol":"XAG-USD","raw_symbol":"XAG"},
 "KXWTIH":{"symbol":"XTI-USD","raw_symbol":"XTI"},
}
REPORT_STEM="commodities-hourly-calibrator"

def policy(family):
 return {"family":family,"symbol":SPECS[family]["symbol"],"horizon":"hourly","minimum_control_edge":.10,"max_entry_all_in_usd":2.0,"entry_price_min":.05,"entry_price_max":.95,"max_report_age_ms":20*60_000,"max_chronos_age_ms":20*60_000,"chronos_source":"chronos2-forecast","cohorts":["chronos_alone","control_alone","agreement","conflict"],"exit":"hold to recorded Kalshi settlement","authority":"paper_research_only_no_order_api"}
def policy_hash(family):return hashlib.sha256(json.dumps(policy(family),sort_keys=True,separators=(",",":")).encode()).hexdigest()
def default_db():return os.path.expanduser("~/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db")
def default_state(family):return common.evidence_path("chronos-%s-shadow.json"%family.lower())
def batch_fee(price,n):return 0 if n<=0 else math.ceil(.07*n*price*(1-price)*100-1e-12)/100
def size(price,cap=2.0):
 n=int(cap//price)
 while n>0 and n*price+batch_fee(price,n)>cap+1e-9:n-=1
 return n
def event_of(ticker):return ticker.rsplit("-T",1)[0] if "-T" in ticker else ticker
def asks(pred):
 out={}
 if pred.get("market_yes_ask") is not None:out["YES"]=float(pred["market_yes_ask"])
 if pred.get("market_yes_bid") is not None:out["NO"]=1-float(pred["market_yes_bid"])
 return out
def control_candidate(family,ticker,pred):
 if not ticker.startswith(family+"-"):return None
 try:p=float(pred["p_yes_full"])
 except (KeyError,TypeError,ValueError):return None
 options=[]
 for side,price in asks(pred).items():
  if policy(family)["entry_price_min"]<=price<=policy(family)["entry_price_max"]:
   options.append(((p-price) if side=="YES" else ((1-p)-price),side,price))
 if not options:return None
 edge,side,price=max(options)
 if edge<policy(family)["minimum_control_edge"]:return None
 return {"ticker":ticker,"event_ticker":event_of(ticker),"control_side":side,"control_price":price,"control_edge":edge,"floor_strike":float((pred.get("features") or {})["floor_strike"]),"spot":float((pred.get("features") or {})["spot"]),"asks":asks(pred)}
def best_candidates(family,predictions):
 best={}
 for ticker,pred in predictions.items():
  row=control_candidate(family,ticker,pred)
  if row and (row["event_ticker"] not in best or row["control_edge"]>best[row["event_ticker"]]["control_edge"]):best[row["event_ticker"]]=row
 return list(best.values())
def initial(family,now):return {"schema":1,"event":"commodity_chronos_shadow_state","family":family,"policy":policy(family),"policy_sha256":policy_hash(family),"frozen_at_ms":int(now),"records":{},"diagnostics":{"runs":0,"skip_reasons":{},"last_run":{}}}
def atomic_write(path,payload):
 os.makedirs(os.path.dirname(os.path.abspath(path)),exist_ok=True);fd,tmp=tempfile.mkstemp(prefix=".commodity-chronos-",dir=os.path.dirname(os.path.abspath(path)),text=True)
 try:
  with os.fdopen(fd,"w",encoding="utf-8") as h:json.dump(payload,h,indent=2,sort_keys=True);h.write("\n");h.flush();os.fsync(h.fileno())
  os.replace(tmp,path)
 finally:
  if os.path.exists(tmp):os.unlink(tmp)
def load(path,family,now):
 if not os.path.exists(path):return initial(family,now),True
 with open(path,encoding="utf-8") as h:s=json.load(h)
 if s.get("policy")!=policy(family) or s.get("policy_sha256")!=policy_hash(family):raise RuntimeError("paper policy mismatch; refusing to tune an existing trial")
 return s,False
def latest_chronos(db_path,family,generated_ms):
 con=sqlite3.connect(f"file:{os.path.abspath(db_path)}?mode=ro",uri=True)
 try:r=con.execute("SELECT id,created_at,direction,features_json FROM edge_decision_journal WHERE source=? AND symbol=? AND horizon='1h' AND created_at<=? AND created_at>=? ORDER BY created_at DESC LIMIT 1",("chronos2-forecast",SPECS[family]["symbol"],generated_ms,generated_ms-policy(family)["max_chronos_age_ms"])).fetchone()
 finally:con.close()
 if not r:return None
 try:f=json.loads(r[3] or "{}")
 except ValueError:f={}
 forecast=f.get("forecast") or f
 return {"journal_id":r[0],"created_at":r[1],"direction":r[2],"predicted_return_bps":float(forecast.get("predicted_return_bps") or f.get("expected_move_bps") or 0),"reference_price":float(forecast.get("last_price") or f.get("reference_price") or 0)}
def diagnostics(state):return state.setdefault("diagnostics",{"runs":0,"skip_reasons":{},"last_run":{}})
def skip(state,reason,count=1):
 if count<=0:return
 d=diagnostics(state);d["skip_reasons"][reason]=d["skip_reasons"].get(reason,0)+count;d["last_run"][reason]=d["last_run"].get(reason,0)+count
def apply_report(state,report,chrono,now):
 generated=int(report.get("generated_at_ms") or 0);family=state["family"];d=diagnostics(state);d["runs"]+=1;d["last_run"]={}
 if generated<state["frozen_at_ms"]:skip(state,"report_before_freeze");return
 if now-generated>policy(family)["max_report_age_ms"]:skip(state,"stale_report");return
 if not chrono:skip(state,"missing_chronos_forecast");return
 predictions=((report.get("by_family") or {}).get(family) or {}).get("predictions") or {}
 d["last_run"]["market_predictions"]=len(predictions)
 candidates=best_candidates(family,predictions);d["last_run"]["eligible_control_candidates"]=len(candidates)
 if not predictions:skip(state,"no_market")
 elif not candidates:skip(state,"no_qualifying_control_edge")
 for row in candidates:
  if row["event_ticker"] in state["records"]:skip(state,"duplicate_event");continue
  endpoint=chrono["reference_price"]*(1+chrono["predicted_return_bps"]/10000)
  chronos_side="YES" if endpoint>row["floor_strike"] else "NO";chronos_price=row["asks"].get(chronos_side)
  if chronos_price is None:skip(state,"missing_chronos_side_ask");continue
  if not policy(family)["entry_price_min"]<=chronos_price<=policy(family)["entry_price_max"]:skip(state,"chronos_price_out_of_range");continue
  cq=size(row["control_price"]);hq=size(chronos_price)
  if cq<1 or hq<1:skip(state,"insufficient_two_dollar_capacity");continue
  state["records"][row["event_ticker"]]={**row,"status":"open","observed_at_ms":now,"report_generated_at_ms":generated,"chronos":{**chrono,"predicted_endpoint":endpoint,"side":chronos_side,"price":chronos_price},"relationship":"agreement" if chronos_side==row["control_side"] else "conflict","control_contracts":cq,"control_all_in":cq*row["control_price"]+batch_fee(row["control_price"],cq),"chronos_contracts":hq,"chronos_all_in":hq*chronos_price+batch_fee(chronos_price,hq)}
  d["last_run"]["opened"]=d["last_run"].get("opened",0)+1
def outcomes(family,settlements=None):
 settlements,_=common.read_jsonl("kalshi-settlements.jsonl") if settlements is None else (settlements,None);out={}
 for r in settlements:
  ticker=str(r.get("kalshi_market_id") or r.get("ticker") or "")
  if not ticker.startswith(family+"-"):continue
  val=str(r.get("result") or r.get("market_result") or "").upper()
  if val in ("YES","NO"):out[ticker]=val=="YES"
 return out
def settle(state,result_map,now):
 for r in state["records"].values():
  if r["status"]!="open" or r["ticker"] not in result_map:continue
  yes=result_map[r["ticker"]];r["outcome_yes"]=yes;r["control_won"]=yes==(r["control_side"]=="YES");r["chronos_won"]=yes==(r["chronos"]["side"]=="YES");r["control_pnl"]=(r["control_contracts"] if r["control_won"] else 0)-r["control_all_in"];r["chronos_pnl"]=(r["chronos_contracts"] if r["chronos_won"] else 0)-r["chronos_all_in"];r["status"]="completed";r["settled_at_ms"]=now;r["postmortem"]={"spot":r["spot"],"floor_strike":r["floor_strike"],"strike_distance":r["spot"]-r["floor_strike"],"relationship":r["relationship"],"control_side":r["control_side"],"chronos_side":r["chronos"]["side"],"control_edge":r["control_edge"],"chronos_predicted_return_bps":r["chronos"]["predicted_return_bps"],"control_price":r["control_price"],"chronos_price":r["chronos"]["price"],"outcome":"YES" if yes else "NO"}
  d=diagnostics(state);d["last_run"]["settled"]=d["last_run"].get("settled",0)+1
def cohort(state,name):
 rows=[r for r in state["records"].values() if r["status"]=="completed" and (name not in ("agreement","conflict") or r["relationship"]==name)];rows.sort(key=lambda r:r["settled_at_ms"]);pk="chronos_pnl" if name=="chronos_alone" else "control_pnl";wk="chronos_won" if name=="chronos_alone" else "control_won";eq=peak=dd=0
 for r in rows:eq+=r[pk];peak=max(peak,eq);dd=max(dd,peak-eq)
 return {"completed":len(rows),"net_pnl":eq,"win_rate":sum(r[wk] for r in rows)/len(rows) if rows else None,"max_drawdown":dd}
def summarize(state):return {"family":state["family"],"authority":policy(state["family"])["authority"],"policy_sha256":state["policy_sha256"],"open":sum(r["status"]=="open" for r in state["records"].values()),"cohorts":{n:cohort(state,n) for n in policy(state["family"])["cohorts"]},"validation_minimum":50,"validation_target":100,"diagnostics":diagnostics(state)}
def run_once(family,path,db_path=None,now_ms=None,report=None,chrono=None,settlements=None):
 now=int(time.time()*1000) if now_ms is None else int(now_ms);db_path=db_path or default_db();os.makedirs(os.path.dirname(os.path.abspath(path)),exist_ok=True);fd=os.open(os.path.abspath(path)+".lock",os.O_CREAT|os.O_RDWR,0o644)
 try:
  fcntl.flock(fd,fcntl.LOCK_EX);state,created=load(path,family,now)
  if not created:
   if report is None:
    with open(common.evidence_path(REPORT_STEM+".json"),encoding="utf-8") as h:report=json.load(h)
   generated=int(report.get("generated_at_ms") or 0);chrono=chrono if chrono is not None else latest_chronos(db_path,family,generated);apply_report(state,report,chrono,now);settle(state,outcomes(family,settlements),now)
  state["updated_at"]=dt.datetime.fromtimestamp(now/1000,tz=dt.timezone.utc).isoformat();atomic_write(path,state);return {"created":created,"state_path":os.path.abspath(path),"policy":policy(family),"summary":summarize(state)}
 finally:fcntl.flock(fd,fcntl.LOCK_UN);os.close(fd)
def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("family",choices=sorted(SPECS));p.add_argument("--json",action="store_true");p.add_argument("--state");p.add_argument("--db",default=default_db());a=p.parse_args(argv);out=run_once(a.family,a.state or default_state(a.family),a.db);print(json.dumps(out,sort_keys=True,indent=None if a.json else 2));return 0
if __name__=="__main__":raise SystemExit(main())
