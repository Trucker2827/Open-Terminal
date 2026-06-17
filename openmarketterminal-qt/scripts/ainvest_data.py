#!/usr/bin/env python3
"""AInvest ownership data (read-only). Currently: US Congress (politician) trades.

Reads AINVEST_API_KEY from the environment (forwarded from the OS keychain by
PythonRunner). Output matches the edgar tools' contract so the GUI can treat it
uniformly:
  success: {"success": true, "ticker": "...", "data": [rows], "count": N}
  failure: {"error": {"command", "error", "type"}}   # incl. "not configured"

Stdlib only (urllib) — no extra deps. Endpoint:
  GET https://openapi.ainvest.com/open/ownership/congress?ticker=&page=&size=
  Authorization: Bearer <AINVEST_API_KEY>
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://openapi.ainvest.com/open/ownership"


def _err(command, msg):
    return {"error": {"command": command, "error": msg, "type": "AInvestError"}}


def _get(path, params, token):
    url = f"{BASE}/{path}?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(
        url, headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.loads(r.read().decode("utf-8"))


def _extract_list(payload):
    """The response envelope's `data` may be a list or a dict wrapping one."""
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict):
        for k in ("list", "items", "records", "trades", "data"):
            v = payload.get(k)
            if isinstance(v, list):
                return v
        for v in payload.values():
            if isinstance(v, list):
                return v
    return []


def congress(ticker, size=50):
    token = os.environ.get("AINVEST_API_KEY", "").strip()
    if not token:
        return _err("congress", "AInvest not configured — add AINVEST_API_KEY in Settings → Credentials")
    try:
        resp = _get("congress", {"ticker": ticker.upper(), "page": 1, "size": size}, token)
    except urllib.error.HTTPError as e:
        return _err("congress", f"AInvest HTTP {e.code}: {e.reason}")
    except Exception as e:  # network/JSON/etc — never crash the GUI's fetch
        return _err("congress", f"AInvest request failed: {type(e).__name__}: {e}")

    sc = resp.get("status_code")
    if sc not in (0, None, "0"):
        return _err("congress", f"AInvest error {sc}: {resp.get('status_msg', 'unknown')}")

    rows = []
    for t in _extract_list(resp.get("data", resp)):
        if not isinstance(t, dict):
            continue
        rows.append({
            "trade_date": t.get("trade_date"),
            "name": t.get("name"),
            "party": t.get("party"),
            "state": t.get("state"),
            "trade_type": t.get("trade_type"),
            "size": t.get("size"),
            "filing_date": t.get("filing_date"),
            "reporting_gap": t.get("reporting_gap"),
        })
    return {"success": True, "ticker": ticker.upper(), "data": rows, "count": len(rows)}


def main(args=None):
    args = args if args is not None else sys.argv[1:]
    if not args:
        print(json.dumps(_err("main", "no command")))
        return
    cmd = args[0]
    if cmd == "congress":
        ticker = args[1] if len(args) > 1 else "AAPL"
        size = int(args[2]) if len(args) > 2 else 50
        print(json.dumps(congress(ticker, size)))
    else:
        print(json.dumps(_err("main", f"unknown command: {cmd}")))


if __name__ == "__main__":
    main()
