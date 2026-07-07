#!/bin/bash
# e2e_sandbox_smoke.sh — Strategy Sandbox Task 11 end-to-end smoke, offline,
# NO GUI, NO daemon process. Drives the built openterminalcli against a
# fully isolated $HOME (a fresh temp dir, not a throwaway profile under the
# real HOME — the sandbox's "default" profile writes straight under $HOME
# with no --profile flag needed, so isolating HOME is simpler and leak-proof
# than the profile-name-in-real-HOME trick the other e2e_*.sh scripts use).
# The app root under $HOME (Library/Application Support/... on macOS,
# .local/share/... on Linux/Windows — see cli/BridgeDiscoveryFile.cpp's
# app_root()) is located AFTER `sandbox seed` by finding the profile DB it
# just created, rather than hardcoding a platform path, so this test passes
# under both the local macOS dev build and the Linux ctest regression gate
# (.github/workflows/regression.yml) — same approach as
# tests/e2e_paper_trade.sh.
#
# Real-horizon reshape (2026-07-07 plan, task 2): the season-1 seed no
# longer registers (and actively RETIRES) the 'scalp' book — sub-minute,
# fee-dead, no venue — so this suite can no longer drive the scalp
# decision+tick fill lane end to end. It is repointed onto the surviving
# 'chronos2' (15m) book instead, which is the cleanest replacement: like
# scalp it opens a concrete (non-hypothetical, non-prediction) paper
# position with real target/stop/expiry math resolved off the SAME tick
# tail file (PaperExecutor.cpp's run_cycle hardcodes ticks_path to
# daemon/scalp_ticks.jsonl for every position kind, not just scalp), and
# chronos2's `horizon_filter` does an EXACT string match against the
# journal row's own `horizon` column (see PaperExecutor.cpp's
# open_price_forecast_candidates) — so, unlike spot/kalshi (which now fan
# out to 3 horizon-variant books per journal row post-reshape, see
# tst_sandbox_executor.cpp's kalshi_seed_book_opens_on_real_producer_source),
# a single chronos2-forecast/horizon='15m' row opens exactly ONE position,
# keeping this suite's "one closed position, hand-computed pnl" shape
# intact. Unlike scalp, chronos2 has no pending_fill stage — it opens
# directly at 'open' state against the journal row's features_
# json.reference_price (no decision+fill-tick two-step) — so this suite's
# fixture setup is simpler than the old scalp version despite driving the
# same real fill->target-exit->resolved-leaderboard contract.
#
# Flow (mirrors PaperExecutor.cpp's open_price_forecast_candidates /
# advance_open_positions and PaperFillModel.cpp's check_exit):
#   1. `sandbox seed` registers the eleven season-1 books (spot 1h/4h/1d,
#      kalshi 15m/1h/1d, long_short, plus Chronos BTC 15m/1h/1d and equity;
#      scalp/btc5m/chronos2_5m are retired, not seeded).
#   2. A fresh chronos2-forecast/horizon=15m/side=buy journal row is
#      inserted directly into edge_decision_journal (via python3's stdlib
#      sqlite3 against the profile DB — there is no scalp-style .jsonl
#      fixture file for this lane; the real producer, `edge chronos2
#      forecast --journal`, hits the network, which this OFFLINE suite must
#      not do), timestamped within the chronos2 book's 1800s max_age_sec
#      window.
#   3. `sandbox tick` #1 opens the position directly at 'open' (no
#      pending_fill stage for price_forecast books) against the journal
#      row's reference_price.
#   4. A target-crossing tick (>= reference_price * (1 + target_bps/10000))
#      is appended to daemon/scalp_ticks.jsonl AFTER cycle 1 (ts_ms >
#      opened_at) so only `sandbox tick` #2 can see it, which then closes
#      the position with close_reason=target.
#   5. `sandbox positions --closed` shows exactly that one closed position.
#   6. `sandbox score-now` + `sandbox leaderboard` show the chronos2 book
#      with resolved=1 and a net_pnl matching a hand-computed figure (gross
#      move against the journal row's reference_price, both entry and exit
#      fee at the taker rate — 60bps — per open_price_forecast_candidates'
#      and advance_open_positions' own fee_model_from_params/fee_for calls)
#      on notional_usd=50.
#   7. `sandbox eligibility` runs clean and reports the chronos2 book
#      BLOCKED (insufficient resolved sample: 1 < kMinResolvedSample=30,
#      plus insufficient active days and no demonstrated edge — a
#      freshly-seeded book cannot be eligible by construction).
#
# This achieves the STRONG level: real open -> real target exit -> resolved
# leaderboard row with a verified hand-computed net_pnl -> eligibility
# blocked. (Not settling for the weaker "opens only" fallback the brief
# allows.)
#
# Timestamp note: the brief's `date +%s%3N` does NOT work on BSD/macOS date
# (no %N support — it prints a literal "N"), and this suite must pass on both
# the local macOS dev build and the Linux ctest regression gate. python3
# (already a hard dependency of every other e2e_*.sh script's JSON
# assertions) gives a portable millisecond clock instead.
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

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

echo "== sandbox e2e smoke (offline, temp HOME=$TMPHOME) =="
echo "CLI=$CLI  watchdog=${TIMEOUT:-none}"
echo

# ============================================================================
# Step 1 — seed the eleven season-1 books.
# ============================================================================
SEED_JSON="$(wd 30 "$CLI" --json sandbox seed)" || fail "sandbox seed exited nonzero"
printf '%s' "$SEED_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert isinstance(d.get('seeded'), list) and len(d['seeded']) == 11, d
" || fail "sandbox seed did not report 11 seeded strategy ids: $SEED_JSON"
echo "PASS: sandbox seed -> 11 strategies"

CHRONOS_ID="$(printf '%s' "$(wd 30 "$CLI" --json sandbox list --status active)" | python3 -c "
import sys, json
d = json.load(sys.stdin)
ids = [s['strategy_id'] for s in d['strategies'] if s['kind'] == 'chronos2']
print(ids[0] if ids else '')
")"
[ -n "$CHRONOS_ID" ] || fail "no active chronos2 (15m) strategy after seed"
echo "PASS: chronos2 (15m) strategy id = $CHRONOS_ID"

# Removed-kind retirement (task 2 of the horizon reshape): scalp/btc5m must
# never be ACTIVE after a seed, regardless of what an older binary may have
# left behind.
printf '%s' "$(wd 30 "$CLI" --json sandbox list --status active)" | python3 -c "
import sys, json
d = json.load(sys.stdin)
kinds = {s['kind'] for s in d['strategies']}
assert 'scalp' not in kinds, d
assert 'btc5m' not in kinds, d
assert 'chronos2_5m' not in kinds, d
" || fail "sandbox seed left a removed-kind book ACTIVE"
echo "PASS: no scalp/btc5m/chronos2_5m book is active after seed"

# ============================================================================
# Locate the app root the CLI actually bootstrapped `sandbox seed` into, by
# finding the profile DB it just created, rather than hardcoding a
# platform-specific path (AppPaths::root()'s mirror in
# cli/BridgeDiscoveryFile.cpp's app_root() differs across macOS/Linux/
# Windows, and this suite must pass under Linux ctest too — see
# .github/workflows/regression.yml). This is the same "derive from the DB
# location" approach tests/e2e_paper_trade.sh uses.
# ============================================================================
PROFILE_DB="$(find "$HOME" -type f -name 'openmarketterminal.db' 2>/dev/null | head -1)"
[ -n "$PROFILE_DB" ] && [ -f "$PROFILE_DB" ] || fail "could not locate openmarketterminal.db under $HOME after sandbox seed"
APP_ROOT="$(dirname "$(dirname "$PROFILE_DB")")" # <app_root>/data/openmarketterminal.db
DAEMON_DIR="$APP_ROOT/daemon"
mkdir -p "$DAEMON_DIR" || fail "could not create $DAEMON_DIR"
echo "PASS: app root = $APP_ROOT"

# ============================================================================
# Step 2 — fixture chronos2-forecast journal row, BEFORE cycle 1.
# chronos2 (15m) book params (SandboxRegistry.cpp seed_default_strategies):
#   notional_usd=50, target_bps=45, stop_bps=25, horizon_sec=900,
#   max_age_sec=1800, journal_source='chronos2-forecast', horizon filter='15m'.
# The book opens DIRECTLY at 'open' state against reference_price (no
# pending_fill/limit-offset stage, unlike the old scalp lane) — see
# PaperExecutor.cpp's open_price_forecast_candidates.
#   entry price  = reference_price
#   target price = reference_price * (1 + 45/10000)
# ============================================================================
REF_PRICE=50000.0
NOW1_MS="$(now_ms)"
DECISION_TS_MS=$((NOW1_MS - 5000)) # well within the 1800s max_age_sec window
DECISION_ID="dec-chronos15m-e2e-1"

python3 -c "
import sqlite3, json
conn = sqlite3.connect('${PROFILE_DB}')
features = json.dumps({'reference_price': ${REF_PRICE}})
freshness = json.dumps({'freshest_age_ms': 50, 'live_sources': 3})
conn.execute(
    'INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, horizon, side, call, gate,'
    ' market_probability, confidence, freshness_json, features_json, source)'
    ' VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)',
    ('${DECISION_ID}', ${DECISION_TS_MS}, ${DECISION_TS_MS}, 'BTC-USD', '15m', 'buy', 'BUY CANDIDATE', 'pass',
     0.0, 0.9, freshness, features, 'chronos2-forecast'))
conn.commit()
conn.close()
" || fail "could not insert chronos2-forecast fixture journal row"
echo "PASS: fixture chronos2-forecast journal row inserted (decision_id=${DECISION_ID})"

# ============================================================================
# Step 3 — cycle 1: `sandbox tick` opens the position directly (no
# pending_fill stage for a price_forecast book).
# ============================================================================
TICK1_JSON="$(wd 30 "$CLI" --json sandbox tick)" || fail "sandbox tick (cycle 1) exited nonzero"
printf '%s' "$TICK1_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('opened') == 1, d
" || fail "cycle 1 did not open exactly one position: $TICK1_JSON"
echo "PASS: cycle 1 opened=1 ($TICK1_JSON)"

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
# can see it (ts_ms > opened_at, price >= target_price). Written to
# daemon/scalp_ticks.jsonl -- PaperExecutor.cpp's run_cycle uses that one
# tick tail file for every position kind, not just scalp.
# ============================================================================
TARGET_TICK_TS_MS=$((OPENED_AT_MS + 500))
TARGET_TICK_PRICE="50300.0" # >= target_price (50225.0 = 50000 * 1.0045) -> triggers a target exit
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
# PaperExecutor/PaperFillModel use (reference_price as entry, target tick
# price as exit, taker entry fee 60bps + taker exit fee 60bps on
# notional_usd=50 -- open_price_forecast_candidates/advance_open_positions
# both charge fee_model_from_params(...).taker_bps, unlike the old scalp
# lane's maker-entry/taker-exit split).
# ============================================================================
CLOSED_JSON="$(wd 30 "$CLI" --json sandbox positions --closed)" || fail "sandbox positions --closed exited nonzero"
EXPECTED_NET_PNL="$(python3 -c "
ref = ${REF_PRICE}
qty = 50.0 / ref
entry_fee = 50.0 * 60.0 / 10000.0
exit_fee = 50.0 * 60.0 / 10000.0
exit_price = ${TARGET_TICK_PRICE}
gross = (exit_price - ref) * qty
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
# Step 6 — score-now + leaderboard: chronos2 (15m) book resolved=1, net_pnl
# matches.
# ============================================================================
wd 30 "$CLI" --json sandbox score-now >/dev/null || fail "sandbox score-now exited nonzero"

LB_JSON="$(wd 30 "$CLI" --json sandbox leaderboard)" || fail "sandbox leaderboard exited nonzero"
printf '%s' "$LB_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [r for r in d['leaderboard'] if r['strategy_id'] == '${CHRONOS_ID}']
assert len(rows) == 1, d
row = rows[0]
assert row['resolved'] == 1, row
expected = ${EXPECTED_NET_PNL}
got = row['net_pnl']
assert abs(got - expected) < 1e-6, (got, expected, row)
assert row['ranked'] is False, row  # resolved 1 << kMinResolvedSample 30
" || fail "leaderboard chronos2 row mismatch: $LB_JSON"
echo "PASS: leaderboard shows chronos2 (15m) book resolved=1, net_pnl matches, ranked=false (insufficient sample)"

# ============================================================================
# Step 7 — eligibility: chronos2 (15m) book runs clean and is BLOCKED
# (insufficient resolved sample, among other freshly-seeded blockers).
# ============================================================================
ELIG_JSON="$(wd 30 "$CLI" --json sandbox eligibility)" || fail "sandbox eligibility exited nonzero"
printf '%s' "$ELIG_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
rows = [r for r in d['eligibility'] if r['strategy_id'] == '${CHRONOS_ID}']
assert len(rows) == 1, d
row = rows[0]
assert row['eligible'] is False, row
assert any('insufficient resolved sample' in b for b in row['blockers']), row
" || fail "eligibility chronos2 row mismatch: $ELIG_JSON"
echo "PASS: eligibility runs clean, chronos2 (15m) book BLOCKED (insufficient resolved sample)"

echo
echo "ALL PASS: sandbox e2e smoke (seed -> journal fixture -> open -> target exit ->"
echo "          closed position -> score/leaderboard -> eligibility blocked)"
exit 0
