#!/bin/bash
#
# e2e_headless_categories.sh — per-category headless smoke for openterminalcli.
#
# For each enabled data category we invoke a representative MCP tool through the
# in-process headless runtime and assert that the call RETURNS without hanging
# or crashing:
#
#   rc == 0          -> returned with data                          (PASS)
#   rc == 5          -> returned a clean tool error                 (PASS)
#                       (no-data / network error / missing-arg validation)
#   rc == 124        -> timeout watchdog fired: the call HUNG       (FAIL)
#   rc >= 134        -> abnormal termination: the call CRASHED      (FAIL)
#   any other rc     -> returned (process exited cleanly)           (PASS)
#
# The gate is "no hang / no crash + a real in-process MCP round-trip", NOT live
# network success: the sandbox may lack network, in which case a clean rc=5 is a
# perfectly good PASS. Each call is wrapped in a `timeout` watchdog.
#
# Categories smoked (9): markets, news, macro/economics, geopolitics, gov-data,
# M&A, edgar, datahub, portfolio. relationship-map is intentionally NOT smoked
# (GUI-tier; not brought up headless).
#
# Usage: bash tests/e2e_headless_categories.sh
#   Override the binary with CLI=/path/to/openterminalcli.

set -u

CLI="${CLI:-/tmp/ot-build-ht/openterminalcli}"
WATCHDOG="${WATCHDOG:-30}"

if [ ! -x "$CLI" ]; then
    echo "FATAL: openterminalcli not found/executable at: $CLI" >&2
    echo "       set CLI=/path/to/openterminalcli to override." >&2
    exit 2
fi

if ! command -v timeout >/dev/null 2>&1; then
    echo "FATAL: 'timeout' not found (install coreutils, e.g. brew install coreutils)." >&2
    exit 2
fi

FAILURES=0

# run <category-label> <tool-name> <json-args>
run() {
    local label="$1" tool="$2" args="$3" rc
    timeout "$WATCHDOG" "$CLI" --headless mcp call "$tool" "$args" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "FAIL: $label HUNG (timeout ${WATCHDOG}s) [$tool]"
        FAILURES=$((FAILURES + 1))
    elif [ "$rc" -ge 134 ]; then
        echo "FAIL: $label CRASHED (rc=$rc) [$tool]"
        FAILURES=$((FAILURES + 1))
    else
        echo "PASS: $label returned (rc=$rc) [$tool]"
    fi
}

echo "== headless per-category smoke (CLI=$CLI, watchdog=${WATCHDOG}s) =="

run "markets"           get_quote               '{"symbol":"AAPL"}'
run "news"              get_news                '{"query":"markets"}'
run "macro/economics"   list_dbnomics_providers '{}'
run "geopolitics"       fetch_geopolitics_events '{}'
run "gov-data"          list_gov_data_providers '{}'
# M&A: ma_dcf is a pure calculator; with no args it returns a clean missing-param
# error (rc=5). This exercises the MCP dispatch round-trip without a live fetch.
run "m&a"               ma_dcf                  '{}'
# edgar: live-fetch tools (edgar_resolve_ticker/find_company with valid args) can
# stall on the SEC endpoint; the dispatch round-trip is smoked via a clean
# missing-param validation error (rc=5).
run "edgar"             edgar_resolve_cik       '{}'
run "datahub"           datahub_list_topics     '{}'
run "portfolio"         list_portfolios         '{}'

echo "== summary =="
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: all categories returned without hang/crash"
    exit 0
else
    echo "FAIL: $FAILURES category(ies) hung or crashed"
    exit 1
fi
