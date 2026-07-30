#!/bin/bash
# e2e_kalshi_grid_cli.sh — the `kalshi grid` verb, proved against the REAL
# openterminalcli, offline, no GUI. It is READ-ONLY: it surfaces the strategy-grid
# engine's kalshi-strategy-grid-latest.json (headline + survivors + closest
# candidates), recomputes nothing, and takes no venue action. Everything runs
# against a temp evidence dir via OPENTERMINAL_KALSHI_EVIDENCE_DIR
# (cli/ServeCommand.h's kalshi_evidence_path), so the operator's real files are
# never read or written.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

fail() { echo "FAIL: $*"; exit 1; }

# A minimal, honest verdict file: no survivors, one positive candidate.
cat > "$EVIDENCE/kalshi-strategy-grid-latest.json" <<'JSON'
{"schema_version":1,"as_of_utc":"2026-07-30T00:00:00+00:00",
 "headline":"no variant beats hold+market after correction",
 "survivors":[],
 "candidates":[{"variant_id":"NO|b50-75|physics|sl20","side":"NO","band":[0.50,0.75],
   "gate":"physics","exit":{"kind":"sl","amount":0.20},"delta_vs_hold":0.082,
   "delta_vs_market":0.076,"effective_n":20,"ci95":[-0.005,0.169],
   "walkforward_delta":0.109,"trust":"insufficient_sample",
   "blocked_by":"effective_n 20 < 30"}]}
JSON

# 1. Human output: the headline + the candidate with its blocked_by reason.
OUT="$("$CLI" kalshi grid)" || fail "kalshi grid exited nonzero"
echo "$OUT" | grep -q "no variant beats hold+market after correction" \
    || fail "human output missing the headline: $OUT"
echo "$OUT" | grep -q "closest candidates" || fail "human output missing candidates section"
echo "$OUT" | grep -q "effective_n 20 < 30" || fail "human output missing blocked_by reason"

# 2. --json passthrough: the raw file, machine-readable.
JSON_OUT="$("$CLI" kalshi grid --json)" || fail "kalshi grid --json exited nonzero"
echo "$JSON_OUT" | grep -q '"schema_version"' || fail "--json did not emit the raw JSON"
echo "$JSON_OUT" | grep -q '"blocked_by"' || fail "--json missing candidate fields"

# 3. Missing file -> UNAVAILABLE, fail-closed (nonzero exit).
rm -f "$EVIDENCE/kalshi-strategy-grid-latest.json"
if "$CLI" kalshi grid > "$EVIDENCE/miss.out" 2>&1; then
    fail "kalshi grid should exit nonzero when the file is missing"
fi
grep -q "UNAVAILABLE" "$EVIDENCE/miss.out" || fail "missing file did not report UNAVAILABLE"

echo "PASS: e2e_kalshi_grid_cli"
