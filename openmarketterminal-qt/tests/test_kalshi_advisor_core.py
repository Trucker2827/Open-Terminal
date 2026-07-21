import json, os, sys, tempfile, unittest

ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0,os.path.join(ROOT,"scripts","kalshi_advise"))
from advisor_core import *
from advisor_loop import pid_alive
from codex_forecaster import PROMPT_VERSION, tool_less_command

class AdvisorCoreTest(unittest.TestCase):
    def test_explicit_abstention_has_no_probability(self):
        a=validate_forecast({"decision":"abstain","reason_code":"WEAK_SIGNAL","confidence":.2})
        self.assertEqual(a["decision"],"abstain");self.assertNotIn("probability",a)
        with self.assertRaises(ValueError):
            validate_forecast({"decision":"abstain","reason_code":"X","probability":.5})

    def test_cost_net_proposal_and_gate_are_deterministic(self):
        m={"market_implied_probability":.50,"yes_bid":.49,"yes_ask":.50,"no_bid":.49,
           "no_ask":.50,"yes_depth":10,"no_depth":10}
        p=build_order_proposal("c","T",.60,.8,m,1000,10000,GatePolicy())
        self.assertEqual(p["side"],"yes");self.assertEqual(p["gate"],"pass")
        self.assertAlmostEqual(p["cost_net_edge"],.08)
        self.assertFalse(p["execution_eligible"]);self.assertEqual(p["execution_mode"],"shadow")

    def test_hash_chain_detects_mutation(self):
        with tempfile.TemporaryDirectory() as d:
            path=os.path.join(d,"j.jsonl");j=ImmutableJournal(path)
            j.append({"opportunity_id":"1"});j.append({"opportunity_id":"2"});self.assertTrue(j.verify())
            rows=j.read();rows[0]["opportunity_id"]="tampered"
            with open(path,"w") as f:
                for r in rows:f.write(json.dumps(r)+"\n")
            self.assertFalse(j.verify())

    def test_qualification_is_versioned_and_fail_closed(self):
        q=qualification({"n_resolved":500,"daemon_comparable":500,
            "headline_improvement_vs_daemon":.02,"improvement_vs_market_pre":.01,"ci_low":.001,
            "net_value_after_fees":{"value":.01}})
        self.assertTrue(q["qualified"]);self.assertEqual(q["policy_version"],QUALIFICATION_POLICY_VERSION)
        self.assertFalse(qualification({})["qualified"])

    def test_missing_pid_is_not_running(self):
        self.assertFalse(pid_alive(0));self.assertFalse(pid_alive(-1))

    def test_safety_is_restart_safe_and_fail_closed(self):
        now=1_800_000_000_000
        s=record_safety_observation({},now_ms=now,reconciled=True,equity=100,open_exposure=1)
        self.assertTrue(evaluate_safety(s,now)["safe"])
        for _ in range(3):s=record_safety_observation(s,now_ms=now,realized_pnl=-1)
        self.assertIn("consecutive_loss_limit",evaluate_safety(s,now)["blockers"])
        restored=json.loads(json.dumps(s))
        self.assertIn("consecutive_loss_limit",evaluate_safety(restored,now)["blockers"])

    def test_promotion_and_canary_require_qualification_and_safety(self):
        now=1_800_000_000_000;safe=evaluate_safety(record_safety_observation({},now_ms=now,reconciled=True),now)
        good={"qualified":True,"policy_version":QUALIFICATION_POLICY_VERSION}
        q=promotion_transition({"state":"SHADOW"},good,safe,"evaluate",now)
        self.assertEqual(q["state"],"QUALIFIED")
        self.assertEqual(promotion_transition(q,good,safe,"enable_canary",now)["state"],"CANARY_ENABLED")
        with self.assertRaises(ValueError):
            promotion_transition({"state":"SHADOW"},{"qualified":False},safe,"enable_canary",now)
        with self.assertRaises(ValueError):
            promotion_transition(q,good,{"safe":False,"blockers":["exposure_limit"]},"enable_canary",now)

    def test_comparative_shadow_uses_same_proposal_interface(self):
        m={"market_implied_probability":.5,"yes_bid":.49,"yes_ask":.5,"no_bid":.49,"no_ask":.5,
           "yes_depth":10,"no_depth":10}
        rows=comparative_proposals("c","T",.62,.56,.8,m,1000,10000)
        self.assertEqual(set(rows),{"advisor","daemon","consensus","disagreement"})
        self.assertEqual(rows["advisor"]["schema_version"],rows["daemon"]["schema_version"])

    def test_codex_epoch_structurally_disables_agent_tools(self):
        cmd=tool_less_command("/tmp/schema","/tmp/empty")
        disabled={cmd[i+1] for i,x in enumerate(cmd[:-1]) if x=="--disable"}
        self.assertTrue({"shell_tool","unified_exec","code_mode_host","apps","browser_use","computer_use"} <= disabled)
        self.assertIn("--ephemeral",cmd);self.assertIn("--ignore-user-config",cmd)
        self.assertEqual(cmd[cmd.index("--cd")+1],"/tmp/empty")
        self.assertIn("tool-less",PROMPT_VERSION)

if __name__=="__main__":unittest.main()
