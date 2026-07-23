#!/usr/bin/env bash
# Issue #91 — tools/sign-release.sh must probe notary credential availability
# BEFORE any download/sign work and fail fast with a screen-lock explanation
# when the keychain profile is unreadable (locked screen -> locked
# data-protection keychain -> misleading 'No Keychain password item found'
# minutes into a run).
#
# Offline and deterministic: `xcrun` and `gh` are stubbed on PATH; the real
# script is executed only up to the probe (failure case) or the first gh call
# (success case). SIGN_IDENTITY is preset so `security` is never consulted.
# Runs under zsh when available (the script's real interpreter, macOS) and
# falls back to bash (Linux CI) — the probe section uses only shared syntax.
set -euo pipefail

# ${BASH_SOURCE[0]:-$0}: BASH_SOURCE is bash-only; under zsh (set -u) it
# aborts with 'parameter not set', so fall back to $0.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
SCRIPT="${SIGN_RELEASE_SCRIPT:-$HERE/../../tools/sign-release.sh}"
[ -f "$SCRIPT" ] || { echo "FAIL: sign-release.sh not found at $SCRIPT" >&2; exit 1; }
SH=$(command -v zsh || command -v bash)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
STUBS="$TMP/stubs"; MARKS="$TMP/marks"
mkdir -p "$STUBS" "$MARKS"

# xcrun stub: `notarytool history` touches a marker and honors STUB_PROBE_RC;
# any other xcrun invocation is unexpected before the download step.
cat > "$STUBS/xcrun" <<'STUB'
#!/usr/bin/env bash
if [ "${1:-}" = "notarytool" ] && [ "${2:-}" = "history" ]; then
  touch "$MARKS/probe-called"
  if [ "${STUB_PROBE_RC:-0}" -ne 0 ]; then
    echo "Error: No Keychain password item found for profile: dinero-notarytool"
    exit "$STUB_PROBE_RC"
  fi
  echo "Successfully received submission history."
  exit 0
fi
touch "$MARKS/xcrun-unexpected"
exit 1
STUB
# gh stub: records that the download step was reached, then fails so the
# success-case run stops before needing hdiutil/codesign.
cat > "$STUBS/gh" <<'STUB'
#!/usr/bin/env bash
touch "$MARKS/gh-called"
exit 1
STUB
chmod +x "$STUBS/xcrun" "$STUBS/gh"

run_script() {  # $1 = probe rc for the xcrun stub
  # Remove markers explicitly: zsh (unlike bash) aborts on an unmatched
  # glob, and $MARKS is empty before the first run.
  rm -f "$MARKS/probe-called" "$MARKS/gh-called" "$MARKS/xcrun-unexpected"
  set +e
  OUT=$(PATH="$STUBS:$PATH" MARKS="$MARKS" STUB_PROBE_RC="$1" \
        SIGN_IDENTITY="Developer ID Application: Test (TEAM123)" \
        "$SH" "$SCRIPT" v0.0.0-test 2>&1)
  RC=$?
  set -e
}

fail() { echo "FAIL: $1" >&2; echo "--- script output ---" >&2; printf '%s\n' "$OUT" >&2; exit 1; }

# Case 1: locked keychain — probe fails, script exits immediately with the
# screen-lock explanation, and the download step is never reached.
run_script 1
[ "$RC" -ne 0 ] || fail "locked-keychain probe did not make the script exit nonzero"
[ -f "$MARKS/probe-called" ] || fail "probe (notarytool history) was never invoked"
[ ! -f "$MARKS/gh-called" ] || fail "download step ran despite a failed credential probe"
printf '%s' "$OUT" | grep -q "screen is locked" || fail "failure message does not explain the screen-lock cause"
printf '%s' "$OUT" | grep -q "unlock the Mac" || fail "failure message does not explain the fix"
printf '%s' "$OUT" | grep -q "No Keychain password item found" || fail "probe output (real notarytool error) not surfaced"
echo "ok: locked keychain -> immediate exit with cause and fix, no download"

# Case 2: credentials readable — probe passes and the script proceeds to the
# existing download step unchanged (gh stub is reached, then aborts the run).
run_script 0
[ -f "$MARKS/probe-called" ] || fail "probe was never invoked on the success path"
[ -f "$MARKS/gh-called" ] || fail "script did not proceed to the download step after a passing probe"
printf '%s' "$OUT" | grep -q "credentials OK" || fail "success path did not report the probe passing"
echo "ok: passing probe -> success path proceeds to download"

echo "PASS: test_sign_release_probe"
