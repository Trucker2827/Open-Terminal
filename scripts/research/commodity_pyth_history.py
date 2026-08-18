#!/usr/bin/env python3
"""Append settlement-aligned Pyth commodity ticks to the Chronos research store.

Gold/silver/WTI are independent. Missing Pyth feeds remain unavailable; Yahoo
futures are deliberately never substituted. Local research data only.
"""
from __future__ import annotations
import argparse, hashlib, json, os, sqlite3, sys, time

ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),"..",".."))
ADVISE=os.path.join(ROOT,"openmarketterminal-qt","scripts","kalshi_advise")
sys.path.insert(0,ADVISE)
import commodities_15m_calibrator as c15

SPECS={
 "KXGOLDH":{"symbol":"XAU","race_family":"KXGOLD15M","pyth_symbol":"Metal.XAU/USD","pyth_id":c15.FAMILIES["KXGOLD15M"]["pyth_id"]},
 "KXSILVERH":{"symbol":"XAG","race_family":"KXSILVER15M","pyth_symbol":"Metal.XAG/USD","pyth_id":c15.FAMILIES["KXSILVER15M"]["pyth_id"]},
 "KXWTIH":{"symbol":"XTI","race_family":"KXWTI15M","pyth_symbol":"Metal.XTI/USD","pyth_id":c15.FAMILIES["KXWTI15M"]["pyth_id"]},
}

def default_db():return os.path.expanduser("~/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db")
def default_seed():return os.path.expanduser("~/Library/Application Support/Open Terminal/Open Terminal/commodities-15m-calibrator-state.json")
def ensure_schema(con):
 con.execute("CREATE TABLE IF NOT EXISTS edge_prediction_raw_ticks (id TEXT PRIMARY KEY,symbol TEXT NOT NULL DEFAULT '',source TEXT NOT NULL DEFAULT '',price REAL NOT NULL DEFAULT 0,exchange_ts INTEGER NOT NULL DEFAULT 0,received_ts INTEGER NOT NULL DEFAULT 0)")
 con.execute("CREATE INDEX IF NOT EXISTS idx_edge_pred_ticks_scope ON edge_prediction_raw_ticks(symbol,received_ts)")

def tick_id(symbol,ts,price):return hashlib.sha256(f"pyth|{symbol}|{ts}|{price:.12f}".encode()).hexdigest()
def insert_tick(con,spec,ts,price,received):
 if int(ts)<=0 or float(price)<=0:return 0
 source="pyth:"+spec["pyth_symbol"]
 cur=con.execute("INSERT OR IGNORE INTO edge_prediction_raw_ticks(id,symbol,source,price,exchange_ts,received_ts) VALUES(?,?,?,?,?,?)",(tick_id(spec["symbol"],ts,price),spec["symbol"],source,float(price),int(ts),int(received)))
 return cur.rowcount

def seed_rows(path):
 if not os.path.exists(path):return {}
 with open(path,encoding="utf-8") as h:state=json.load(h)
 out={}
 for family,spec in SPECS.items():
  branch=(state.get("by_family") or {}).get(spec["race_family"]) or {}
  out[family]=((branch.get("pyth_series") or {}).get(spec["pyth_symbol"]) or [])
 return out

def collect(db_path=None,seed_path=None,now_ms=None,latest=None):
 db_path=db_path or default_db();seed_path=seed_path or default_seed();now_ms=int(time.time()*1000) if now_ms is None else int(now_ms)
 if latest is None:
  try:latest=c15.fetch_pyth_latest([s["pyth_id"] for s in SPECS.values()])
  except Exception:latest={}
 con=sqlite3.connect(db_path);ensure_schema(con);seeded=seed_rows(seed_path);status={}
 try:
  for family,spec in SPECS.items():
   inserted=0
   for ts,price in seeded.get(family,[]):inserted+=insert_tick(con,spec,ts,price,now_ms)
   current=latest.get(spec["pyth_id"])
   if current:
    price,_confidence,publish_ms=current;inserted+=insert_tick(con,spec,publish_ms,price,now_ms)
   count,newest=con.execute("SELECT COUNT(*),MAX(exchange_ts) FROM edge_prediction_raw_ticks WHERE symbol=? AND source=?",(spec["symbol"],"pyth:"+spec["pyth_symbol"])).fetchone()
   ready=bool(current)
   status[family]={"symbol":spec["symbol"],"source":"pyth:"+spec["pyth_symbol"],"inserted":inserted,"stored":count,"newest_ms":newest or 0,"status":"READY" if ready else "WAITING_FOR_SETTLEMENT_FEED","source_policy":"authoritative_settlement_aligned_only_no_substitutes","blocker":None if ready else "configured Pyth feed returned no current observation"}
  con.commit()
 finally:con.close()
 return {"event":"commodity_pyth_history","authority":"research_data_only_no_order_api","families":status}

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("--json",action="store_true");p.add_argument("--db",default=default_db());p.add_argument("--seed",default=default_seed());a=p.parse_args(argv)
 print(json.dumps(collect(a.db,a.seed),sort_keys=True,indent=None if a.json else 2));return 0
if __name__=="__main__":raise SystemExit(main())
