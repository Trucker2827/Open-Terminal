#!/bin/bash
# e2e_kalshi_bot_lessons.sh — issue #174, criterion 3 (and the CLI half of 2),
# proved against the REAL openterminalcli, offline, no GUI and no daemon.
#
# tst_kalshi_edge_lessons proves the formatter in process and
# test_kalshi_edge_report proves the reducer that feeds it. What neither can
# prove is that the SHIPPED CLI renders that formatter over the published
# artifact, which is exactly what "two renderers, one artifact — the parity
# rule" claims. So this asserts, against the binary:
#
#   * an ABSENT artifact renders one refusal line, NO digits from a record that
#     has none, and a non-zero exit — never an empty card that reads as
#     "nothing to learn";
#   * a published artifact renders one line per lesson, each carrying its
#     VERDICT and its SAMPLE SIZE in that question's own unit — including the
#     INSUFFICIENT_DATA lesson, which must be present rather than dropped;
#   * a lesson whose payload states no sample size reads SAMPLE SIZE NOT
#     STATED, never a zero;
#   * a STALE artifact says so on EVERY line and paints no line green — a
#     stale EDGE is amber, which is the whole freshness treatment;
#   * an artifact dated in the FUTURE is stale too, not clamped to fresh;
#   * the human lines and the JSON `lines` array are IDENTICAL, because both
#     come from kalshi_edge_lessons() — the same function the BOT panel's
#     `view.lessons` and the cockpit's `scene.lessons` are built from.
#
# --refresh is deliberately NOT exercised here: it spawns the research scripts
# against whatever evidence exists and takes minutes. What it publishes is
# covered by test_kalshi_edge_report; what the CLI does with a published file is
# covered here. The artifacts below are FIXTURES and are labelled as such — no
# number in this script is presented as a measurement.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real artifact is never read or written.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
HELPERS="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE" "$HELPERS"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

REPORT="$EVIDENCE/kalshi-edge-report.json"

fail() { echo "FAIL: $*"; exit 1; }

has_digit() { grep -qE '[0-9]' <<< "$1"; }

# Writes a fixture artifact whose generated_at_ms is $1 milliseconds from now
# (negative = in the past). Four lessons, one per verdict the enum carries, so
# every rendering branch is exercised by one file.
write_report() {
    python3 - "$REPORT" "$1" <<'PY'
import json, sys, time
path, offset_ms = sys.argv[1], int(sys.argv[2])
generated = int(time.time() * 1000) + offset_ms


def lesson(qid, title, verdict, n, unit, detail=None):
    sample = {"unit": unit, "detail": detail}
    if n is not None:
        sample["n"] = n
    return {
        "id": qid, "title": title, "verdict": verdict,
        "claim": "a fixture claim for %s" % qid,
        "verdict_reason": "a fixture reason",
        "key_numbers": [{"label": "drift", "value": 0.0122, "text": "+1.22c"}],
        "sample": sample,
        "data_span": {"text": "9.0h of record"},
    }


json.dump({
    "schema": 1, "event": "kalshi_edge_report",
    "generated_at_ms": generated,
    "lessons": [
        lesson("Q1", "QUOTE LAG VS SPOT", "FEE_EATEN", 22, "spot moves at 2.0sigma"),
        lesson("Q2", "CALIBRATOR VS THE MARKET MID", "NO_EDGE", 271, "settled contracts"),
        lesson("Q3", "RECALIBRATION HEADROOM", "EDGE", 251, "out-of-sample contracts"),
        lesson("Q4", "THE BOT'S OWN SETTLED BIDS", "INSUFFICIENT_DATA", None, "settled positions"),
    ],
}, open(path, "w"), indent=2)
PY
}

# The human lines and the JSON `lines` array must be the same strings. Asserted
# for every artifact state below, because "one formatter" is a property of each
# branch, not of the happy path.
assert_parity() {
    local human="$1" jsonf="$2"
    python3 - "$human" "$jsonf" <<'PY'
import json, sys
human = [l.rstrip("\n") for l in open(sys.argv[1], encoding="utf-8")]
out = json.load(open(sys.argv[2], encoding="utf-8"))
lines = out["lines"]
# The human renderer prints the same lines, then its own two footer lines
# (the artifact's path and the display-only disclaimer), which carry no
# lesson content.
assert human[:len(lines)] == lines, (
    "human and --json disagree:\n  human: %r\n  json:  %r" % (human[:len(lines)], lines))
PY
}

# --- the ABSENT artifact, before anything is published -----------------------
set +e
"$CLI" kalshi bot lessons > "$HELPERS/absent.txt" 2>&1
rc=$?
set -e
[ "$rc" -eq 3 ] || fail "an absent artifact exited $rc, expected 3"
grep -q "WHAT THE RECORD TEACHES · UNAVAILABLE" "$HELPERS/absent.txt" ||
    fail "an absent artifact did not render the UNAVAILABLE refusal"
# Exactly one lesson line: the refusal. The two footer lines are the path and
# the disclaimer, neither of which is a lesson.
if grep -qE "^Q[0-9] " "$HELPERS/absent.txt"; then
    fail "an absent artifact rendered lesson lines anyway"
fi
# No digits anywhere except the file path the refusal names.
refusal="$(head -1 "$HELPERS/absent.txt")"
if has_digit "${refusal#*kalshi-edge-report.json}"; then
    fail "an absent artifact printed numbers after naming the file: $refusal"
fi

set +e
"$CLI" --json kalshi bot lessons > "$HELPERS/absent.json" 2>/dev/null
set -e
python3 - "$HELPERS/absent.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["available"] is False, out
assert out["lessons"] == [], out
assert "unavailable_reason" in out, out
assert "report" not in out, "an absent artifact published a report object"
PY

# --- a FRESH artifact --------------------------------------------------------
write_report 0
"$CLI" kalshi bot lessons > "$HELPERS/fresh.txt" 2>&1 ||
    fail "a published artifact exited non-zero"
"$CLI" --json kalshi bot lessons > "$HELPERS/fresh.json" ||
    fail "--json kalshi bot lessons exited non-zero on a published artifact"
assert_parity "$HELPERS/fresh.txt" "$HELPERS/fresh.json"

grep -q "4 lessons from the edge autopsy" "$HELPERS/fresh.txt" ||
    fail "the header did not state how many lessons were rendered"
for q in Q1 Q2 Q3 Q4; do
    grep -qE "^$q " "$HELPERS/fresh.txt" || fail "$q is missing from the rendered card"
done
# Criterion 2: every line carries its sample size, in its own unit.
grep -q "n=22 spot moves at 2.0sigma" "$HELPERS/fresh.txt" ||
    fail "Q1 did not carry its sample size in spot moves"
grep -q "n=271 settled contracts" "$HELPERS/fresh.txt" ||
    fail "Q2 did not carry its sample size in contracts"
# The INSUFFICIENT_DATA lesson is present, and states its absence rather than
# a zero — the two failures this criterion is really about.
grep -q "Q4 .*INSUFFICIENT_DATA" "$HELPERS/fresh.txt" ||
    fail "the INSUFFICIENT_DATA lesson was dropped from the card"
grep -q "SAMPLE SIZE NOT STATED" "$HELPERS/fresh.txt" ||
    fail "a lesson with no sample size did not say SAMPLE SIZE NOT STATED"
if grep -q "n=0" "$HELPERS/fresh.txt"; then
    fail "a lesson with no sample size rendered a zero"
fi
# A fresh artifact does not shout STALE, and its EDGE line is green.
if grep -q "STALE" "$HELPERS/fresh.txt"; then
    fail "a fresh artifact rendered STALE"
fi
python3 - "$HELPERS/fresh.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["available"] is True and out["stale"] is False, out
roles = {l["id"]: l["role"] for l in out["lessons"]}
assert roles["Q3"] == "green", roles      # EDGE, vouched-for age
assert roles["Q1"] == "amber", roles      # FEE_EATEN
assert roles["Q4"] == "grey", roles       # INSUFFICIENT_DATA
# The artifact's own object travels verbatim for machine readers.
assert out["report"]["schema"] == 1, out["report"]
PY

# --- a STALE artifact: 30 days old -------------------------------------------
write_report "-$((30 * 24 * 3600 * 1000))"
"$CLI" kalshi bot lessons > "$HELPERS/stale.txt" 2>&1 ||
    fail "a stale artifact exited non-zero"
"$CLI" --json kalshi bot lessons > "$HELPERS/stale.json" ||
    fail "--json exited non-zero on a stale artifact"
assert_parity "$HELPERS/stale.txt" "$HELPERS/stale.json"

# Every line, not only the header — a conclusion read on its own must read stale.
while IFS= read -r line; do
    case "$line" in
        "WHAT THE RECORD TEACHES"*|Q[0-9]*)
            grep -q "STALE" <<< "$line" ||
                fail "a stale artifact left a line without its age: $line" ;;
    esac
done < "$HELPERS/stale.txt"
python3 - "$HELPERS/stale.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["stale"] is True, out
roles = [l["role"] for l in out["lessons"]]
assert "green" not in roles, "a stale artifact painted a line green: %r" % roles
# The verdict word itself is unchanged — only the colour is.
verdicts = {l["id"]: l["verdict"] for l in out["lessons"]}
assert verdicts["Q3"] == "EDGE", verdicts
PY

# --- an artifact dated in the FUTURE is mistrusted, not clamped --------------
write_report "$((24 * 3600 * 1000))"
"$CLI" --json kalshi bot lessons > "$HELPERS/future.json" ||
    fail "--json exited non-zero on a future-dated artifact"
python3 - "$HELPERS/future.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["stale"] is True, "a future-dated artifact read as fresh: %r" % out["header"]
assert "FUTURE" in out["header"], out["header"]
assert "green" not in [l["role"] for l in out["lessons"]], out["lessons"]
PY

# --- an UNPARSEABLE artifact reads unavailable, not empty --------------------
printf '{ this is not json' > "$REPORT"
set +e
"$CLI" kalshi bot lessons > "$HELPERS/broken.txt" 2>&1
rc=$?
set -e
[ "$rc" -eq 3 ] || fail "an unparseable artifact exited $rc, expected 3"
grep -q "not parseable JSON" "$HELPERS/broken.txt" ||
    fail "an unparseable artifact did not say why it was refused"
if grep -qE "^Q[0-9] " "$HELPERS/broken.txt"; then
    fail "an unparseable artifact rendered lesson lines anyway"
fi

# --- a FAILED --refresh renders the previous artifact through BOTH renderers -
# The publisher is unreachable here (a working directory with no repo under it
# and no python3 on PATH), so --refresh fails. What must NOT happen: the card
# vanishing, the two renderers behaving differently, or the previous artifact
# being presented as if the refresh had worked.
write_report 0
( cd "$HELPERS" && PATH=/nonexistent-for-this-test \
    "$CLI" kalshi bot lessons --refresh > "$HELPERS/refresh-fail.txt" 2> "$HELPERS/refresh-fail.err"
  echo "$?" > "$HELPERS/refresh-fail.rc" ) || true
[ "$(cat "$HELPERS/refresh-fail.rc")" != "0" ] ||
    fail "a --refresh that could not run exited 0"
grep -q "NOT refreshed" "$HELPERS/refresh-fail.err" ||
    fail "a failed --refresh did not say so on stderr"
# The card is still rendered, with the artifact's real age.
grep -q "WHAT THE RECORD TEACHES · 4 lessons" "$HELPERS/refresh-fail.txt" ||
    fail "a failed --refresh suppressed the previous artifact instead of rendering it"

( cd "$HELPERS" && PATH=/nonexistent-for-this-test \
    "$CLI" --json kalshi bot lessons --refresh > "$HELPERS/refresh-fail.json" 2>/dev/null
  echo "$?" > "$HELPERS/refresh-fail-json.rc" ) || true
[ "$(cat "$HELPERS/refresh-fail-json.rc")" = "$(cat "$HELPERS/refresh-fail.rc")" ] ||
    fail "--json and the human renderer disagreed on a failed refresh's exit code"
assert_parity "$HELPERS/refresh-fail.txt" "$HELPERS/refresh-fail.json"
python3 - "$HELPERS/refresh-fail.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
# The artifact is real and is rendered; the failure is a stated field, not an
# exit code a stdout-only reader would miss.
assert out["available"] is True, out
assert out["refreshed"] is False, "a failed refresh was not stated in the JSON: %r" % out
PY

# --- an unknown option is refused, not ignored -------------------------------
set +e
"$CLI" kalshi bot lessons --nonsense > "$HELPERS/badopt.txt" 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "an unknown option exited $rc, expected 2"

echo "PASS: kalshi bot lessons renders the artifact through one formatter — absent,"
echo "      fresh, stale, future-dated and unparseable — with the sample size on"
echo "      every line and no green on a stale card."
