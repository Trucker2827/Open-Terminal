#!/usr/bin/env python3
"""Firewalled Codex CLI forecaster. Codex receives only stdin blind context."""
import json, os, re, subprocess, sys, tempfile

MODEL=os.environ.get("KALSHI_CODEX_MODEL","gpt-5.6-sol")
PROMPT_VERSION="kalshi-blind-codex-v2-tool-less"
PROMPT=("Estimate P(YES) for this Kalshi crypto contract using ONLY the supplied price-free JSON. "
"You cannot browse or use tools. Return one JSON object: either "
'{"decision":"predict","probability":0..1,"confidence":0..1,"rationale":"one sentence"} or '
'{"decision":"abstain","reason_code":"INSUFFICIENT_EVIDENCE","confidence":0..1,"rationale":"one sentence"}.')

def tool_less_command(schema_path, isolated_cwd):
    return ["codex","exec","--ephemeral","--ignore-user-config","--ignore-rules",
            "--skip-git-repo-check","--sandbox","read-only","--model",MODEL,"--cd",isolated_cwd,
            "--disable","shell_tool","--disable","unified_exec","--disable","code_mode_host",
            "--disable","apps","--disable","browser_use","--disable","computer_use",
            "--disable","image_generation","--output-schema",schema_path,"-"]

def main():
    mode=sys.argv[1] if len(sys.argv)>1 else "predict"
    if mode=="identify":
        print(json.dumps({"provider":"openai-codex-cli","model":MODEL,"prompt_version":PROMPT_VERSION,
                          "temperature":-1,"agent_id":"codex-unattended/"+PROMPT_VERSION}));return 0
    if mode!="predict":return 2
    try:ctx=json.loads(sys.stdin.read())
    except Exception as exc:print(json.dumps({"error":str(exc)}),file=sys.stderr);return 2
    schema={"type":"object","properties":{"decision":{"type":"string","enum":["predict","abstain"]},
      "probability":{"type":["number","null"]},"confidence":{"type":"number"},"rationale":{"type":"string"},
      "reason_code":{"type":["string","null"]}},
      "required":["decision","probability","confidence","rationale","reason_code"],"additionalProperties":False}
    with tempfile.TemporaryDirectory(prefix="kalshi-codex-blind-") as isolated:
        sp=os.path.join(isolated,"response-schema.json")
        with open(sp,"w") as f:json.dump(schema,f)
        command=tool_less_command(sp,isolated)
        env=dict(os.environ)
        r=subprocess.run(command,input=PROMPT+"\n\nContext:\n"+json.dumps(ctx,sort_keys=True),capture_output=True,text=True,timeout=50,env=env)
    text=(r.stdout or "").strip();matches=re.findall(r"\{.*\}",text,re.S)
    if r.returncode or not matches:
        print(json.dumps({"decision":"abstain","reason_code":"CODEX_UNAVAILABLE","confidence":0,
                          "rationale":((r.stderr or "")+" "+(r.stdout or "Codex returned no structured output"))[-800:]}));return 0
    parsed=json.loads(matches[-1])
    if parsed.get("decision")=="abstain": parsed.pop("probability",None)
    else: parsed.pop("reason_code",None)
    print(json.dumps(parsed));return 0
if __name__=="__main__":raise SystemExit(main())
