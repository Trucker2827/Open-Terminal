#!/usr/bin/env python3
"""Layer 2 of the terminal<->Quant Lab bridge: LIVE reads over the MCP bridge.

Talks to the running app/daemon through the same local HTTP bridge the CLI
uses (endpoint + token from bridge.json). Gives every Quant Lab script
optional live access to the terminal's data plane — candles, order books,
DataHub topics — without its own exchange connections.

Read-only by convention: this client is for data tools; it deliberately has
no helper for any order/settings tool, and nothing here should ever be
handed a destructive tool name.

Env override (tests): OPENTERMINAL_BRIDGE_JSON.
"""
import json
import os
import urllib.request

DEFAULT_BRIDGE_JSON = os.path.expanduser(
    "~/Library/Application Support/org.openterminal.OpenTerminal/bridge.json")


class BridgeUnavailable(RuntimeError):
    """The app/daemon is not running (or its bridge file is stale)."""


def _bridge_info():
    path = os.environ.get("OPENTERMINAL_BRIDGE_JSON", DEFAULT_BRIDGE_JSON)
    try:
        with open(path, encoding="utf-8") as fh:
            info = json.load(fh)
    except (OSError, ValueError) as exc:
        raise BridgeUnavailable(
            f"bridge discovery file unreadable ({path}) — is the app running?") from exc
    endpoint = info.get("endpoint")
    token = info.get("token")
    if not endpoint or not token:
        raise BridgeUnavailable(f"bridge discovery file has no endpoint/token ({path})")
    return endpoint, token


def call_tool(name, args=None, timeout_sec=30):
    """POST /tool — returns the tool's JSON result dict.

    Raises BridgeUnavailable when the app isn't reachable and RuntimeError
    when the bridge rejects the call, so callers can fall back to
    terminal_data's offline reads (or fail honestly) instead of fabricating.
    """
    endpoint, token = _bridge_info()
    body = json.dumps({"tool": name, "args": args or {}}).encode("utf-8")
    req = urllib.request.Request(
        endpoint.rstrip("/") + "/tool", data=body, method="POST",
        headers={"Content-Type": "application/json", "X-MCP-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except (OSError, ValueError) as exc:
        raise BridgeUnavailable(f"bridge call failed ({exc}) — is the app running?") from exc
    if isinstance(payload, dict) and payload.get("success") is False:
        raise RuntimeError(f"tool {name} failed: {payload.get('error', 'unknown error')}")
    return payload


def get_candles(symbol, granularity="ONE_MINUTE", limit=300, timeout_sec=30):
    return call_tool("get_candles",
                     {"symbol": symbol, "granularity": granularity, "limit": limit},
                     timeout_sec)


def get_order_book(symbol, timeout_sec=30):
    return call_tool("get_order_book", {"symbol": symbol}, timeout_sec)


def bridge_available():
    try:
        _bridge_info()
        return True
    except BridgeUnavailable:
        return False
