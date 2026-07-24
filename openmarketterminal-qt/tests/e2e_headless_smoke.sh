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
# --json is a separate global flag (parsed alongside --headless/--profile, see
# CommandDispatch.cpp's GlobalOpts parsing); the advise block below needs
# machine-readable output to assert on JSON keys, so it uses HLJ instead of HL.
HLJ=(--json --headless --profile "$PROFILE")
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

# 8) advise ledger → valid JSON with read_only, exit 0 (works on empty DB) ───
out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise ledger 2>/dev/null); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null \
   && printf '%s' "$out" | grep -q '"read_only"'; then
  ok "advise ledger: valid JSON with read_only (exit 0)"
else bad "advise ledger rc=$rc out=[$out]"; fi

# 9) advise score → valid JSON with evidence, exit 0 (works with 0 resolved) ─
out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise score 2>/dev/null); rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null \
   && printf '%s' "$out" | grep -q '"evidence"'; then
  ok "advise score: valid JSON with evidence (exit 0)"
else bad "advise score rc=$rc out=[$out]"; fi

# 10) advise open --ticker KXBTC → NO live Kalshi data in CI, so this must
#     handle BOTH outcomes without hard-failing on absent data:
#       (a) a blind `context` object was produced (live snapshot existed) --
#           assert PRICE_WITHHELD and grep the WHOLE JSON line for every
#           forbidden price/probability key. Any hit is a firewall breach and
#           MUST fail the smoke regardless of data availability.
#       (b) no `context` (unavailable/available:false) -- acceptable pass,
#           logged, not a smoke failure: CI has no live Kalshi feed.
out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise open --ticker KXBTC 2>/dev/null); rc=$?
forbidden_hit=""
for key in market_implied_probability yes_ask yes_bid no_ask no_bid fair_yes fair_no \
           divergence daemon_probability model_weight cost_net_edge; do
  if printf '%s' "$out" | grep -q "\"$key\""; then forbidden_hit="$key"; break; fi
done
if [ -n "$forbidden_hit" ]; then
  bad "advise open: FIREWALL BREACH — forbidden key '$forbidden_hit' present in emitted context/output"
elif printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null \
     && printf '%s' "$out" | grep -q '"context"'; then
  withheld=$(printf '%s' "$out" | python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get("PRICE_WITHHELD", False))
except Exception:
    print(False)' 2>/dev/null)
  if [ $rc -eq 0 ] && [ "$withheld" = "True" ]; then
    ok "advise open: blind context produced, PRICE_WITHHELD, no forbidden keys (exit 0)"
  else
    bad "advise open: context present but PRICE_WITHHELD missing/false (rc=$rc) out=[$out]"
  fi
elif printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
  echo "INFO: advise open returned no live snapshot (rc=$rc) — acceptable, no Kalshi data in CI: [$out]"
  ok "advise open: no live snapshot, unavailable response handled cleanly (no firewall breach)"
else
  bad "advise open: non-JSON output (rc=$rc) out=[$out]"
fi

# 11) advise commit-blind → reveal → commit-post cycle over a locally-opened
#     challenge. This does NOT depend on live Kalshi data: OpenParams can be
#     seeded even when the snapshot path above returned unavailable, because
#     advise open's failure mode there is "no snapshot", not "no CLI path" --
#     so instead we drive the cycle only when advise open (step 10) actually
#     produced a challenge_id; if it didn't (no live data), this block is
#     skipped and logged rather than forced, since commit-blind/reveal/
#     commit-post all require a real challenge_id from a prior open().
challenge_id=""
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
  challenge_id=$(printf '%s' "$out" | python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get("challenge_id",""))
except Exception:
    print("")' 2>/dev/null)
fi
if [ -n "$challenge_id" ]; then
  commit_id="e2e-smoke-$$"
  cb_out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise commit-blind \
    --challenge "$challenge_id" --commit-id "$commit_id" --probability 0.5 2>&1); cb_rc=$?
  rv_out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise reveal --challenge "$challenge_id" 2>&1); rv_rc=$?
  cp_out=$(run 30 "$CLI" "${HLJ[@]}" kalshi auto advise commit-post \
    --challenge "$challenge_id" --commit-id "${commit_id}-post" --probability 0.5 2>&1); cp_rc=$?
  cp_state=$(printf '%s' "$cp_out" | python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get("state",""))
except Exception:
    print("")' 2>/dev/null)
  if [ $cb_rc -eq 0 ] && [ $rv_rc -eq 0 ] && [ $cp_rc -eq 0 ] && [ "$cp_state" = "COMMITTED_POST" ]; then
    ok "advise commit-blind -> reveal -> commit-post: full cycle reached COMMITTED_POST (exit 0)"
  else
    bad "advise cycle failed: commit-blind rc=$cb_rc[$cb_out] reveal rc=$rv_rc[$rv_out] commit-post rc=$cp_rc[$cp_out]"
  fi
else
  echo "INFO: advise open produced no challenge_id (no live snapshot) — skipping commit-blind/reveal/commit-post cycle"
fi

echo
echo "== SUMMARY: $pass passed, $fail failed =="
if [ $fail -eq 0 ]; then echo "PASS: headless e2e smoke"; exit 0; else echo "FAIL: headless e2e smoke"; exit 1; fi
