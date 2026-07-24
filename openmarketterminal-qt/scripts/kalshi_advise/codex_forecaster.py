#!/usr/bin/env python3
"""Firewalled Codex CLI forecaster. Codex receives only stdin blind context."""
import hashlib, json, os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blind_prompt import PROMPT_VERSION, build_prompt, prompt_hash

MODEL=os.environ.get("KALSHI_CODEX_MODEL","gpt-5.6-sol")
EPOCH_ID="kalshi-blind-codex-v4-zero-capability-latency-neutral"
EFFORT=os.environ.get("KALSHI_CODEX_EFFORT", "medium")
TIMEOUT_S=88
CODEX_VERSION="codex-cli 0.144.6"
FEATURE_REGISTRY_SHA256="f515b7f6cedf7806cf8636f45cd2a6ef447cc48d4f9440b33b1a7d26904766d7"

def validate_capability_inventory(version_text, listing_text, *, expected_version=CODEX_VERSION,
                                  expected_digest=FEATURE_REGISTRY_SHA256):
    """Validate a registry snapshot and return flags disabling every capability."""
    if version_text.strip() != expected_version:
        raise RuntimeError("CODEX_CAPABILITY_INVENTORY_CHANGED:version")
    normalized="\n".join(" ".join(line.split()) for line in listing_text.splitlines() if line.strip())+"\n"
    if hashlib.sha256(normalized.encode()).hexdigest() != expected_digest:
        raise RuntimeError("CODEX_CAPABILITY_INVENTORY_CHANGED:features")
    disabled=[]
    for line in normalized.splitlines():
        match=re.fullmatch(r"(\S+) (.+) (true|false)",line)
        if not match:
            raise RuntimeError("CODEX_CAPABILITY_INVENTORY_CHANGED:format")
        name,status,_default=match.groups()
        if status != "removed": disabled.extend(["--disable",name])
    return disabled

def locked_down_features():
    """Query Codex and fail closed unless its complete capability inventory is pinned."""
    try:
        version=subprocess.run(["codex","--version"],capture_output=True,text=True,timeout=5)
        listed=subprocess.run(["codex","features","list"],capture_output=True,text=True,timeout=5)
    except (OSError,subprocess.SubprocessError) as exc:
        raise RuntimeError("CODEX_CAPABILITY_INVENTORY_UNAVAILABLE") from exc
    if version.returncode or listed.returncode:
        raise RuntimeError("CODEX_CAPABILITY_INVENTORY_UNAVAILABLE")
    return validate_capability_inventory(version.stdout,listed.stdout)

def tool_less_command(schema_path, isolated_cwd, lockdown_flags=None):
    if lockdown_flags is None:lockdown_flags=locked_down_features()
    return ["codex","exec","--ephemeral","--ignore-user-config","--ignore-rules",
            "--skip-git-repo-check","--sandbox","read-only","--model",MODEL,"--cd",isolated_cwd,
            *lockdown_flags,"--output-schema",schema_path,"-"]

def main():
    mode=sys.argv[1] if len(sys.argv)>1 else "predict"
    if mode=="identify":
        print(json.dumps({"provider":"openai-codex-cli","model":MODEL,"prompt_version":PROMPT_VERSION,
                          "epoch_id":EPOCH_ID,"effort":EFFORT,"temperature":-1,
                          "timeout_ms":TIMEOUT_S*1000,
                          "agent_id":"codex-unattended/"+EPOCH_ID}));return 0
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
        try:command=tool_less_command(sp,isolated)
        except Exception as exc:
            print(json.dumps({"decision":"abstain","reason_code":"CAPABILITY_LOCKDOWN_FAILED","confidence":0,
                              "rationale":str(exc)[:400]}));return 0
        env=dict(os.environ)
        try:
            r=subprocess.run(command,input=build_prompt(ctx),capture_output=True,text=True,timeout=TIMEOUT_S,env=env)
        except subprocess.TimeoutExpired:
            print(json.dumps({"decision":"abstain","reason_code":"FORECAST_TIMEOUT","confidence":0,
                              "rationale":f"Codex exceeded the {TIMEOUT_S}s internal blind prediction budget",
                              "prompt_hash":prompt_hash(ctx),"epoch_id":EPOCH_ID}));return 0
    text=(r.stdout or "").strip();matches=re.findall(r"\{.*\}",text,re.S)
    if r.returncode or not matches:
        print(json.dumps({"decision":"abstain","reason_code":"CODEX_UNAVAILABLE","confidence":0,
                          "rationale":((r.stderr or "")+" "+(r.stdout or "Codex returned no structured output"))[-800:]}));return 0
    parsed=json.loads(matches[-1])
    parsed["prompt_hash"] = prompt_hash(ctx)
    parsed["epoch_id"] = EPOCH_ID
    if parsed.get("decision")=="abstain": parsed.pop("probability",None)
    else: parsed.pop("reason_code",None)
    print(json.dumps(parsed));return 0
if __name__=="__main__":raise SystemExit(main())
