#!/bin/bash
# e2e_kalshi_bot_status_mode.sh — issue #155, criteria 2 and 3 (and the CLI leg
# of 4), proved against the REAL openterminalcli, offline, no GUI and no daemon.
#
# `kalshi bot status` reported the literal `"mode":"paper"` (KalshiBotCommands.cpp,
# before this issue) while the GUI badge derived the word from the newest
# readable tick and failed closed to UNKNOWN. A bot ticking live was therefore
# reported as PAPER by the shell and LIVE by the window — the CLI called real
# money paper. The classifier now lives in KalshiBotRuntime.h and both surfaces
# render it; tst_kalshi_bot_panel_presentation asserts the BADGE is that
# function's answer, and this asserts the SHIPPED CLI is too:
#
#   2. --json reports the DERIVED mode — `live` for a live newest tick, `paper`
#      for a paper one, `unknown` for a newer row this build cannot read (never
#      `paper`, never `live`), and NO `mode` key at all when nothing states one;
#   3. the human output prints that word on its headline line in the panel's own
#      `[LIVE]` / `[PAPER]` / `[UNKNOWN]` form, and an UNKNOWN carries the same
#      reason sentence and launchctl hint the panel shows.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger is never read or written. `kalshi bot status` is
# read-only — it takes no venue action, runs no tick, and needs no kill switch.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
HELPERS="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE" "$HELPERS"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"

fail() { echo "FAIL: $*"; exit 1; }

# Writes a ledger for one case. Rows are fresh (the loop reads RUNNING), so the
# mode word is the only thing under test here.
write_ledger() {
    python3 - "$LEDGER" "$1" <<'PY'
import json, sys, time
path, case = sys.argv[1], sys.argv[2]
now = int(time.time() * 1000)

def tick(offset, mode=None, event="kalshi_bot_decision", ticker="KXBTC15M-E2E"):
    row = {"event": event, "ts_ms": now - offset, "ticker": ticker,
           "action": "pass", "reason_code": "EDGE_BELOW_THRESHOLD",
           "signal_trusted": True}
    if mode is not None:
        row["mode"] = mode
    return row

if case == "live":
    # A paper tick, then a LIVE one: the newest tick is what is reported.
    rows = [tick(40000, "paper"), tick(20000, "live")]
elif case == "paper":
    # ...and the reverse: a live tick an hour back does not make this one live.
    rows = [tick(3600000, "live"), tick(20000, "paper")]
elif case == "unknown":
    # A row NEWER than the live tick that this build cannot read at all.
    rows = [tick(20000, "live"), tick(5000, None, event="kalshi_bot_tick")]
elif case == "no-tick":
    # Dated rows, a running loop, and nothing that states a mode: paper
    # settlements are the same vintage but are not ticks.
    rows = [{"event": "kalshi_bot_paper_settlement", "ts_ms": now - 20000,
             "mode": "paper", "position_id": "KXBTC15M-E2E@1", "ticker": "KXBTC15M-E2E",
             "side": "YES", "contracts": 4, "won": True, "realized_pnl": 1.93}]
else:
    raise SystemExit(f"unknown case {case}")

with open(path, "w") as handle:
    for row in rows:
        handle.write(json.dumps(row) + "\n")
PY
}

check_json() {
    python3 - "$HELPERS/status.json" "$1" <<'PY'
import json, sys
out, expected = json.load(open(sys.argv[1])), sys.argv[2]
if expected == "":
    # Absent, NOT `paper` — the same absent-is-absent rule last_decision_age_ms
    # follows. A script must not be able to read "paper" off a record in which
    # no tick ever said so.
    assert "mode" not in out, f"a record stating no mode published mode={out['mode']!r}"
else:
    assert out.get("mode") == expected, f"mode is {out.get('mode')!r}, expected {expected!r}"
# The loop is fresh in every case, so a wrong mode can never be blamed on the
# status classifier having bailed out early.
assert out["state"] == "running", out["state"]
PY
}

for case in live paper unknown no-tick; do
    write_ledger "$case"
    "$CLI" --json kalshi bot status > "$HELPERS/status.json" ||
        fail "--json kalshi bot status exited non-zero on the $case ledger"
    "$CLI" kalshi bot status > "$HELPERS/status.txt" 2>&1 ||
        fail "kalshi bot status exited non-zero on the $case ledger"

    case "$case" in
      live)
        # --- criterion 2 ---------------------------------------------------
        check_json live || fail "the live ledger did not report mode=live"
        # The regression this issue exists for, stated plainly: before the fix
        # this same ledger printed "mode":"paper" from a constant.
        if grep -q '"mode":"paper"' "$HELPERS/status.json"; then
            fail "a LIVE ledger was reported as paper by --json"
        fi
        # --- criterion 3 ---------------------------------------------------
        head -1 "$HELPERS/status.txt" | grep -q '^\[LIVE\] BOT ' ||
            fail "the human headline does not carry [LIVE]: $(head -1 "$HELPERS/status.txt")"
        ;;
      paper)
        check_json paper || fail "the paper ledger did not report mode=paper"
        head -1 "$HELPERS/status.txt" | grep -q '^\[PAPER\] BOT ' ||
            fail "the human headline does not carry [PAPER]: $(head -1 "$HELPERS/status.txt")"
        ;;
      unknown)
        check_json unknown || fail "an unreadable newest row was not reported unknown"
        if grep -q '"mode":"paper"' "$HELPERS/status.json"; then
            fail "an unreadable newest row was reported as paper"
        fi
        if grep -q '"mode":"live"' "$HELPERS/status.json"; then
            fail "an unreadable newest row was reported as live"
        fi
        # An UNKNOWN is not a bare word: it carries why nothing is claimed and
        # the launchctl command that fixes it, exactly as the panel prints.
        head -1 "$HELPERS/status.txt" | grep -q '^\[UNKNOWN\] BOT ' ||
            fail "the human headline does not carry [UNKNOWN]: $(head -1 "$HELPERS/status.txt")"
        head -1 "$HELPERS/status.txt" |
            grep -q 'the newest ledger row carries no mode this build can read, so no mode is claimed' ||
            fail "an UNKNOWN mode was printed without its reason"
        head -1 "$HELPERS/status.txt" |
            grep -q 'launchctl kickstart -k gui/\$UID/org.openterminal.kalshi-bot' ||
            fail "an UNKNOWN mode was printed without the launchctl hint"
        ;;
      no-tick)
        check_json "" || fail "a record stating no mode still published one"
        # No badge at all — an invented [PAPER] here would be the same lie in
        # the human renderer that the JSON constant was.
        head -1 "$HELPERS/status.txt" | grep -q '^BOT ' ||
            fail "a record stating no mode still printed a badge: $(head -1 "$HELPERS/status.txt")"
        ;;
    esac
done

# --- the whole point, as one property ---------------------------------------
# Nothing anywhere in the four cases produced a `paper` the record did not
# state. The old constant would have printed one in three of them.
echo "PASS: e2e_kalshi_bot_status_mode — live / paper / unknown / no-tick, --json and human"
