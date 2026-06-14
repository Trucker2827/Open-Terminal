#!/usr/bin/env bash
# Headless end-to-end smoke test for openterminalcli (Phase-2a).
#
# Runs the CLI in --headless mode (in-process via HeadlessRuntime, NO GUI
# running) and asserts the core command surface returns correctly WITHOUT
# hanging. Every command is wrapped in a watchdog `timeout` so a hang FAILS the
# script instead of blocking CI.
#
# Environment-sensitive assertions are tagged [NET]: `quote` depends on
# yfinance/network reachability and is allowed to return either fresh data
# (exit 0) or a no-data / rate-limited result (exit 5). The point of that case
# is that the in-process round-trip RETURNS rather than hangs — Yahoo may 429.
#
# Exit-code contract (see src/cli/CommandDispatch.cpp):
#   0 ok | 2 usage | 3 no-instance | 4 unauthorized | 5 tool-error/denied
#   6 client-error | 7 headless-init-failed | 124 watchdog-killed (hang)
#
# Usage:  tests/e2e_headless_smoke.sh        # uses /tmp/ot-build-ht/openterminalcli
#         OT_CLI=/path/to/openterminalcli tests/e2e_headless_smoke.sh
set -u

# ── Locate the CLI binary ────────────────────────────────────────────────────
CLI="${OT_CLI:-/tmp/ot-build-ht/openterminalcli}"
[ -x "$CLI" ] || { echo "FATAL: CLI not found/executable at '$CLI' (set OT_CLI)"; exit 1; }

# ── Watchdog: timeout / gtimeout ─────────────────────────────────────────────
# macOS has no native `timeout`; Homebrew coreutils provides `gtimeout`. If
# neither exists we run without a watchdog and warn — a hang would then block,
# so CI hosts should install coreutils.
if   command -v timeout  >/dev/null 2>&1; then TIMEOUT="timeout"
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT="gtimeout"
else TIMEOUT=""; echo "WARN: no timeout/gtimeout found — running WITHOUT a hang watchdog (install coreutils for CI safety)"; fi
run() { local secs="$1"; shift; if [ -n "$TIMEOUT" ]; then "$TIMEOUT" "$secs" "$@"; else "$@"; fi; }

# ── Isolated profile ─────────────────────────────────────────────────────────
# Run against a throwaway profile so the smoke test never touches the user's
# real default data and never risks flipping a capability gate. On exit we
# remove BOTH the profile data dir AND our entry from the default profile
# manifest (profiles.json, which lives in root() above the per-profile dir), so
# the run leaves the registry exactly as it found it.
APP_ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
PROFILE="e2e-smoke-$$"
HL=(--headless --profile "$PROFILE")
ROOT="$APP_ROOT/profiles/$PROFILE"
cleanup() {
  rm -rf "$ROOT"; rm -f "/tmp/ot_e2e_quote.$$"
  local mf="$APP_ROOT/profiles.json"
  if [ -f "$mf" ] && command -v python3 >/dev/null 2>&1; then
    python3 - "$mf" "$PROFILE" <<'PY' 2>/dev/null || true
import json, sys
mf, prof = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(mf))
    d["profiles"] = [p for p in d.get("profiles", []) if p.get("name") != prof]
    json.dump(d, open(mf, "w"), indent=4)
except Exception:
    pass
PY
  fi
}
trap cleanup EXIT

pass=0; fail=0
ok()  { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

echo "== openterminalcli headless e2e smoke =="
echo "CLI=$CLI"
echo "profile=$PROFILE  watchdog=${TIMEOUT:-none}"
echo

# 1) version → exit 0 ─────────────────────────────────────────────────────────
out=$(run 15 "$CLI" "${HL[@]}" version 2>&1); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "openterminalcli"; then
  ok "version (exit 0)"
else bad "version rc=$rc out=[$out]"; fi

# 2) status → reports headless/in-process, exit 0 ────────────────────────────
out=$(run 15 "$CLI" "${HL[@]}" status 2>&1); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -qi "headless"; then
  ok "status reports headless/in-process (exit 0)"
else bad "status rc=$rc out=[$out]"; fi

# 3) mcp list → non-empty catalog WITH get_quote, WITHOUT navigate_to_tab ─────
out=$(run 30 "$CLI" "${HL[@]}" mcp list 2>/dev/null); rc=$?
has_quote=$(printf '%s' "$out" | grep -c '"name": "get_quote"')
has_nav=$(printf '%s'   "$out" | grep -c '"name": "navigate_to_tab"')
if [ $rc -eq 0 ] && [ "$has_quote" -ge 1 ] && [ "$has_nav" -eq 0 ]; then
  ok "mcp list has get_quote, lacks navigate_to_tab (exit 0)"
else bad "mcp list rc=$rc get_quote=$has_quote navigate_to_tab=$has_nav"; fi

# 4) mcp describe get_quote → schema, exit 0 ─────────────────────────────────
out=$(run 30 "$CLI" "${HL[@]}" mcp describe get_quote 2>/dev/null); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q '"inputSchema"'; then
  ok "mcp describe get_quote (exit 0, schema present)"
else bad "mcp describe rc=$rc out=[$out]"; fi

# 5) [NET] quote AAPL → exit 0 (data) OR 5 (no-data/rate-limited); never hang ─
#    NOT piped — a pipe would mask the exit code (timeout's rc lives in $?).
run 45 "$CLI" "${HL[@]}" quote AAPL >"/tmp/ot_e2e_quote.$$" 2>&1; rc=$?
if [ $rc -eq 0 ] || [ $rc -eq 5 ]; then
  ok "[NET] quote AAPL returned (rc=$rc — in-process round-trip did not hang)"
elif [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
  bad "[NET] quote AAPL HUNG — watchdog killed it (rc=$rc)"
else
  bad "[NET] quote AAPL rc=$rc (expected 0 or 5); out=[$(cat "/tmp/ot_e2e_quote.$$" 2>/dev/null)]"
fi

# 6) hub topics → JSON, exit 0 (empty result is fine); never hang/crash ───────
out=$(run 30 "$CLI" "${HL[@]}" hub topics 2>/dev/null); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q '"success"'; then
  ok "hub topics returned JSON (exit 0)"
else bad "hub topics rc=$rc out=[$out]"; fi

# 7) GATE (default-deny) → destructive tool DENIED with cli.allow_trading off ─
#    exit 5 ALONE is insufficient: every unsuccessful tool returns 5 (no-data,
#    bad-account, unknown-tool...). So also require the fail-closed denial
#    reason on stderr, which ONLY the gate's deny path emits. If trading were
#    (wrongly) enabled the tool would RUN and fail later with a different
#    message, so this assertion genuinely catches a gate regression.
out=$(run 30 "$CLI" "${HL[@]}" mcp call live_cancel_all_orders '{}' 2>&1); rc=$?
if [ $rc -eq 5 ] && printf '%s' "$out" | grep -qi "requires authenticated auth"; then
  ok "GATE default-deny: live_cancel_all_orders DENIED (exit 5, fail-closed)"
else
  bad "GATE: expected exit 5 + 'requires authenticated auth', got rc=$rc out=[$out]"
fi

echo
echo "== SUMMARY: $pass passed, $fail failed =="
if [ $fail -eq 0 ]; then echo "PASS: headless e2e smoke"; exit 0; else echo "FAIL: headless e2e smoke"; exit 1; fi
