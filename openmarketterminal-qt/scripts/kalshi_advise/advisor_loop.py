#!/usr/bin/env python3
"""Restart-safe unattended Kalshi shadow advisor loop (never submits orders)."""
from __future__ import annotations
import argparse, calendar, fcntl, json, os, plistlib, signal, subprocess, sys, time, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from advise_challenge import (DEFAULT_CLI, DEFAULT_EVIDENCE, DEFAULT_FORECASTER,
    advise_commit_blind, advise_open, contains_forbidden_deep, forecaster_identify, pick_auto_ticker, _run)
from advisor_core import (GatePolicy, ImmutableJournal, ShadowExecutionAdapter,
    comparative_proposals, default_safety_state, evaluate_safety, promotion_transition,
    qualification, record_safety_observation, validate_forecast, write_state)


def paths(profile):
    app = os.path.expanduser("~/Library/Application Support/org.openterminal.OpenTerminal")
    root = os.path.join(app if profile == "default" else os.path.join(app,"profiles",profile), "daemon")
    return {k: os.path.join(root, v) for k, v in {
        "state":"advisor_loop_state.json", "journal":"advisor_opportunities.jsonl",
        "safety":"advisor_safety_state.json", "promotion":"advisor_promotion_state.json",
        "canary":"advisor_canary_config.json", "config":"advisor_loop_config.json",
        "lock":"advisor_loop.lock", "pid":"advisor_loop.pid", "log":"advisor_loop.log"}.items()}

def launch_agent_path(profile):
    return os.path.expanduser(f"~/Library/LaunchAgents/org.openterminal.kalshi-advisor.{profile}.plist")


def cli_json(cli, profile, args, timeout=45):
    cmd=[cli,"--json","--headless","--profile",profile]+args
    r=_run(cmd, timeout=timeout)
    if not r.stdout.strip(): raise RuntimeError(r.stderr.strip() or "CLI returned no JSON")
    out=json.loads(r.stdout)
    if r.returncode: raise RuntimeError(out.get("reason") or out.get("error") or r.stderr.strip())
    return out

def pid_alive(pid):
    if pid <= 0: return False
    try: os.kill(pid,0); return True
    except OSError: return False

def read_obj(path, default):
    try:return json.load(open(path))
    except (OSError,json.JSONDecodeError):return default

def current_control(args):
    p=paths(args.profile); now=int(time.time()*1000)
    safety_state=read_obj(p["safety"],default_safety_state())
    safety=evaluate_safety(safety_state,now)
    identity=forecaster_identify(args.forecaster)
    score_args=["kalshi","auto","advise","score"]
    if identity.get("agent_id"):score_args += ["--forecaster-id",str(identity["agent_id"])]
    if identity.get("provider"):score_args += ["--provider",str(identity["provider"])]
    if identity.get("model"):score_args += ["--model",str(identity["model"])]
    score=cli_json(args.cli,args.profile,score_args)
    qual=qualification(score);promotion=read_obj(p["promotion"],{"state":"SHADOW"})
    return p,safety_state,safety,score,qual,promotion

def refresh_live_safety(args):
    p=paths(args.profile);now=int(time.time()*1000);state=read_obj(p["safety"],{})
    live=cli_json(args.cli,args.profile,["kalshi","auto","live","status"])
    positions=cli_json(args.cli,args.profile,["kalshi","auto","positions"])
    engine=live.get("decision_engine") or {};reconcile_age=int(str(engine.get("last_account_reconcile_age_ms","-1")))
    active_exposure=sum(float(row.get("stake",row.get("cost",0)) or 0)
                        for row in positions.get("active_positions",[]))
    state=record_safety_observation(state,now_ms=now,
        open_exposure=active_exposure,
        submission_unknown_count=int(live.get("submission_unknown_count",0)),
        reconciled=0 <= reconcile_age <= 30_000)
    exact=[]
    today=time.strftime("%Y-%m-%d",time.gmtime(now/1000))
    for row in positions.get("closed_bets",[]):
        pnl=row.get("realized_pnl")
        if isinstance(pnl,(int,float)):exact.append((str(row.get("settled_time","")),float(pnl)))
    exact.sort()
    canary_cfg=read_obj(p["canary"],{})
    epoch_ms=int(canary_cfg.get("epoch_started_at_ms",0) or 0)
    scoped=[]
    for ts,pnl in exact:
        try:ts_ms=int(calendar.timegm(time.strptime(ts[:19],"%Y-%m-%dT%H:%M:%S")))*1000
        except (ValueError,OverflowError):ts_ms=0
        if epoch_ms > 0 and ts_ms >= epoch_ms:scoped.append((ts,pnl))
    state["daily_realized_pnl"]=sum(p for ts,p in exact if ts.startswith(today))
    streak=0
    for _,pnl in reversed(scoped):
        if pnl < 0:streak+=1
        else:break
    state["consecutive_losses"]=streak
    cumulative=peak=drawdown=0.0
    for _,pnl in scoped:
        cumulative+=pnl;peak=max(peak,cumulative);drawdown=max(drawdown,peak-cumulative)
    state["equity_current"]=cumulative;state["equity_peak"]=peak;state["maximum_drawdown"]=drawdown
    state["drawdown_scope"]="canary_epoch";state["canary_epoch_started_at_ms"]=epoch_ms
    state["pnl_reconciliation_pending"]=int(positions.get("closed_pnl_pending_reconciliation",0))
    if not live.get("decision_engine_operational",False):
        state["paused"]=True;state["pause_reason"]="decision_engine_not_operational"
    elif state.get("pause_reason")=="decision_engine_not_operational":
        state["paused"]=False;state["pause_reason"]=""
    write_state(p["safety"],state);return state

def evaluate_and_persist(args):
    p=paths(args.profile)
    try:refresh_live_safety(args)
    except Exception:pass
    _,_,safety,_,qual,current=current_control(args)
    next_state=promotion_transition(current,qual,safety,"evaluate",int(time.time()*1000))
    write_state(p["promotion"],next_state)
    if next_state["state"] in ("PAUSED","DEMOTED"):
        cfg=read_obj(p["canary"],{});cfg["enabled"]=False;cfg["auto_disabled_reason"]=next_state["reason"]
        cfg["updated_at_ms"]=int(time.time()*1000);write_state(p["canary"],cfg)
    return next_state


def run_once(args, journal):
    now=int(time.time()*1000); identity=forecaster_identify(args.forecaster)
    ticker=pick_auto_ticker(args.evidence,args.auto_min_secs_left,args.auto_max_age_s)
    base={"event":"shadow_opportunity","opportunity_id":str(uuid.uuid4()),"opened_at_ms":now,
          "ticker":ticker,"forecaster":identity,"loop_version":"kalshi-advisor-loop-v1"}
    if not ticker:
        return journal.append({**base,"status":"ABSTAINED","reason_code":"NO_FRESH_CONTRACT"})
    opened=advise_open(args.cli,args.profile,ticker,identity)
    if opened.get("available") is False:
        return journal.append({**base,"status":"ABSTAINED","reason_code":"OPEN_UNAVAILABLE",
                               "detail":opened.get("reason")})
    ctx=opened.get("context") or {}; leak=contains_forbidden_deep(ctx)
    if leak: raise RuntimeError(f"FIREWALL_BREACH:{leak}")
    challenge=opened["challenge_id"]; base.update(challenge_id=challenge,context_hash=opened["context_hash"])
    r=_run([sys.executable,args.forecaster,"predict"],stdin_text=json.dumps(ctx),timeout=args.forecast_timeout)
    if r.returncode: return journal.append({**base,"status":"ABSTAINED","reason_code":"FORECASTER_ERROR",
                                            "detail":(r.stderr or r.stdout)[:400]})
    try: forecast=validate_forecast(json.loads(r.stdout))
    except Exception as exc:
        return journal.append({**base,"status":"ABSTAINED","reason_code":"MALFORMED_FORECAST",
                               "detail":str(exc)})
    if forecast["decision"] == "abstain":
        return journal.append({**base,"status":"ABSTAINED","forecast":forecast})
    elapsed=int(time.time()*1000)-int(opened["ts_opened"])
    if elapsed > int(opened["prediction_ttl_ms"])-args.safety_margin_ms:
        return journal.append({**base,"status":"ABSTAINED","reason_code":"PREDICTION_TTL_EXPIRED",
                               "forecast":forecast,"elapsed_ms":elapsed})
    commit=str(uuid.uuid4())
    committed=advise_commit_blind(args.cli,args.profile,challenge,commit,forecast["probability"],
                                   forecast["confidence"],forecast["rationale"])
    revealed=cli_json(args.cli,args.profile,["kalshi","auto","advise","reveal","--challenge",challenge])
    market=revealed.get("withheld_market") or {}
    proposals=comparative_proposals(challenge,ticker,forecast["probability"],None,forecast["confidence"],
                                    market,int(time.time()*1000),int(opened["ts_opened"])+
                                    int(opened["execution_relevance_ms"]))
    proposal=proposals["advisor"]
    execution=ShadowExecutionAdapter().execute(proposal)
    return journal.append({**base,"status":"SHADOW_PROPOSED" if execution["simulated"] else "GATE_REJECTED",
                           "forecast":forecast,"journal_id":committed.get("id"),
                           "proposal":proposal,"comparative_proposals":proposals,"execution":execution})


def report(args, journal):
    p,safety_state,safety,score,qual,promotion=current_control(args)
    return {"loop_version":"kalshi-advisor-loop-v1","journal_valid":journal.verify(),
            "opportunities":len(journal.read()),"qualification":qual,"promotion":promotion,
            "safety":safety,"safety_state":safety_state,"canary":read_obj(p["canary"],{"enabled":False}),"score":score}


def foreground(args):
    p=paths(args.profile); os.makedirs(os.path.dirname(p["lock"]),exist_ok=True)
    with open(p["lock"],"w") as lock:
        try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: raise SystemExit("advisor loop already running")
        with open(p["pid"],"w") as f:f.write(str(os.getpid()))
        journal=ImmutableJournal(p["journal"]); failures=0
        try:
            while True:
                try:
                    row=run_once(args,journal); failures=0
                    promotion=evaluate_and_persist(args)
                    state={"running":True,"pid":os.getpid(),"updated_at_ms":int(time.time()*1000),
                           "last_event_hash":row["event_hash"],"last_status":row["status"],"failures":0,
                           "promotion_state":promotion["state"],"heartbeat_at_ms":int(time.time()*1000)}
                except Exception as exc:
                    failures+=1; state={"running":True,"pid":os.getpid(),"updated_at_ms":int(time.time()*1000),
                        "last_status":"ERROR","last_error":str(exc)[:500],"failures":failures}
                    if failures >= args.maximum_failures:
                        state.update(running=False,last_status="PAUSED_ERROR",pause_reason="maximum_failures")
                        write_state(p["state"],state);return 7
                write_state(p["state"],state)
                if args.once:return 0
                time.sleep(min(args.interval_seconds*(2**min(failures,4)),300))
        finally:
            try:os.unlink(p["pid"])
            except FileNotFoundError:pass


def main():
    ap=argparse.ArgumentParser(); ap.add_argument("command",choices=["once","run","start","stop","status","report",
        "install","uninstall","safety-observe","evaluate","pause","resume","canary-configure","canary-enable","canary-disable","canary-pulse"])
    ap.add_argument("--profile",default="default");ap.add_argument("--cli",default=DEFAULT_CLI)
    ap.add_argument("--forecaster");ap.add_argument("--evidence",default=DEFAULT_EVIDENCE)
    ap.add_argument("--interval-seconds",type=int,default=60);ap.add_argument("--forecast-timeout",type=int,default=50)
    ap.add_argument("--maximum-failures",type=int,default=5)
    ap.add_argument("--safety-margin-ms",type=int,default=6000);ap.add_argument("--auto-min-secs-left",type=int,default=75)
    ap.add_argument("--auto-max-age-s",type=float,default=11.0)
    ap.add_argument("--realized-pnl",type=float,default=0);ap.add_argument("--equity",type=float)
    ap.add_argument("--open-exposure",type=float);ap.add_argument("--submission-unknown-count",type=int)
    ap.add_argument("--reconciled",action="store_true");ap.add_argument("--max-order-dollars",type=float,default=2)
    ap.add_argument("--max-open-exposure",type=float,default=5);ap.add_argument("--daily-loss-limit",type=float,default=5)
    args=ap.parse_args();args.once=args.command=="once"
    p=paths(args.profile)
    saved_config=read_obj(p["config"],{})
    args.forecaster=args.forecaster or saved_config.get("forecaster") or DEFAULT_FORECASTER
    journal=ImmutableJournal(p["journal"])
    if args.command in ("once","run"):return foreground(args)
    if args.command=="install":
        plist=launch_agent_path(args.profile);os.makedirs(os.path.dirname(plist),exist_ok=True)
        label=f"org.openterminal.kalshi-advisor.{args.profile}"
        spec={"Label":label,"ProgramArguments":[sys.executable,os.path.abspath(__file__),"run","--profile",args.profile,
              "--cli",os.path.abspath(args.cli),"--forecaster",os.path.abspath(args.forecaster),"--evidence",args.evidence,
              "--interval-seconds",str(args.interval_seconds),"--maximum-failures",str(args.maximum_failures)],
              "RunAtLoad":True,"KeepAlive":{"SuccessfulExit":False},"ThrottleInterval":30,
              "StandardOutPath":p["log"],"StandardErrorPath":p["log"]}
        with open(plist,"wb") as f:plistlib.dump(spec,f)
        write_state(p["config"],{"forecaster":os.path.abspath(args.forecaster),"cli":os.path.abspath(args.cli),
                                  "profile":args.profile,"installed_at_ms":int(time.time()*1000)})
        uid=str(os.getuid());subprocess.run(["launchctl","bootout",f"gui/{uid}/{label}"],capture_output=True)
        r=subprocess.run(["launchctl","bootstrap",f"gui/{uid}",plist],capture_output=True,text=True)
        print(json.dumps({"installed":r.returncode==0,"label":label,"plist":plist,"error":r.stderr.strip()}));return 0 if r.returncode==0 else 7
    if args.command=="uninstall":
        plist=launch_agent_path(args.profile);label=f"org.openterminal.kalshi-advisor.{args.profile}"
        subprocess.run(["launchctl","bootout",f"gui/{os.getuid()}/{label}"],capture_output=True)
        try:os.unlink(plist)
        except FileNotFoundError:pass
        print(json.dumps({"installed":False,"label":label}));return 0
    if args.command=="start":
        try:
            existing=int(open(p["pid"]).read())
            if pid_alive(existing): print(json.dumps({"started":False,"reason":"already running","pid":existing}));return 3
        except (OSError,ValueError): pass
        os.makedirs(os.path.dirname(p["log"]),exist_ok=True); log=open(p["log"],"a")
        write_state(p["config"],{"forecaster":os.path.abspath(args.forecaster),"cli":os.path.abspath(args.cli),
                                  "profile":args.profile,"started_at_ms":int(time.time()*1000)})
        cmd=[sys.executable,__file__,"run","--profile",args.profile,"--cli",args.cli,"--forecaster",args.forecaster,
             "--evidence",args.evidence,"--interval-seconds",str(args.interval_seconds)]
        proc=subprocess.Popen(cmd,stdout=log,stderr=log,start_new_session=True);print(json.dumps({"started":True,"pid":proc.pid}));return 0
    if args.command=="stop":
        try: pid=int(open(p["pid"]).read());os.kill(pid,signal.SIGTERM);print(json.dumps({"stopped":True,"pid":pid}));return 0
        except (OSError,ValueError):print(json.dumps({"stopped":False,"reason":"not running"}));return 3
    if args.command=="status":
        try: state=json.load(open(p["state"]))
        except (OSError,json.JSONDecodeError):state={"running":False}
        pid=int(state.get("pid",0) or 0);state["running"]=pid_alive(pid)
        state.update(journal_valid=journal.verify(),opportunities=len(journal.read()));print(json.dumps(state));return 0
    if args.command=="safety-observe":
        state=record_safety_observation(read_obj(p["safety"],{}),now_ms=int(time.time()*1000),
            realized_pnl=args.realized_pnl,equity=args.equity,open_exposure=args.open_exposure,
            submission_unknown_count=args.submission_unknown_count,reconciled=args.reconciled)
        write_state(p["safety"],state);print(json.dumps({"state":state,"gate":evaluate_safety(state,int(time.time()*1000))}));return 0
    if args.command in ("evaluate","pause","resume","canary-enable","canary-disable"):
        if args.command in ("evaluate","canary-enable"):
            try:refresh_live_safety(args)
            except Exception:pass
        _,_,safety,score,qual,current=current_control(args)
        action={"evaluate":"evaluate","pause":"pause","resume":"resume","canary-enable":"enable_canary",
                "canary-disable":"disable_canary"}[args.command]
        if args.command=="canary-enable" and not read_obj(p["canary"],{}).get("schema_version"):
            print(json.dumps({"ok":False,"reason":"configure canary before enabling"}));return 6
        try:next_state=promotion_transition(current,qual,safety,action,int(time.time()*1000))
        except ValueError as exc:print(json.dumps({"ok":False,"reason":str(exc),"qualification":qual,"safety":safety}));return 6
        write_state(p["promotion"],next_state)
        if args.command in ("canary-enable","canary-disable"):
            cfg=read_obj(p["canary"],{});cfg["enabled"]=args.command=="canary-enable";cfg["updated_at_ms"]=int(time.time()*1000);write_state(p["canary"],cfg)
        print(json.dumps({"ok":True,"promotion":next_state,"qualification":qual,"safety":safety}));return 0
    if args.command=="canary-configure":
        if min(args.max_order_dollars,args.max_open_exposure,args.daily_loss_limit)<=0:
            print(json.dumps({"ok":False,"reason":"canary limits must be positive"}));return 2
        if args.max_order_dollars > 2 or args.max_open_exposure > 5 or args.daily_loss_limit > 5:
            print(json.dumps({"ok":False,"reason":"v1 canary hard caps are $2/order, $5 exposure, $5 daily loss"}));return 2
        cfg={"schema_version":"kalshi-canary-config-v1","enabled":False,"max_order_dollars":args.max_order_dollars,
             "max_open_exposure":args.max_open_exposure,"daily_loss_limit":args.daily_loss_limit,
             "limit_orders_only":True,"epoch_started_at_ms":int(time.time()*1000),
             "updated_at_ms":int(time.time()*1000)}
        write_state(p["canary"],cfg);print(json.dumps({"ok":True,"canary":cfg}));return 0
    if args.command=="canary-pulse":
        _,_,safety,_,qual,promotion=current_control(args);cfg=read_obj(p["canary"],{"enabled":False})
        blockers=[]
        if not cfg.get("enabled"):blockers.append("canary_disabled")
        if promotion.get("state")!="CANARY_ENABLED":blockers.append("promotion_not_enabled")
        if not qual.get("qualified"):blockers.append("qualification_failed")
        blockers+=safety.get("blockers",[])
        if blockers:print(json.dumps({"submitted":False,"status":"rejected","blockers":blockers}));return 6
        out=cli_json(args.cli,args.profile,["kalshi","auto","live","execute-next"])
        print(json.dumps({"submitted":True,"result":out}));return 0
    print(json.dumps(report(args,journal),sort_keys=True));return 0

if __name__=="__main__":raise SystemExit(main())
