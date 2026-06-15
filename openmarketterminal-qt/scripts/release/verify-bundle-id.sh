#!/bin/bash
# Release guard — assert the built .app's CFBundleIdentifier matches the frozen
# value before we sign or ship.
#
# WHY THIS MATTERS: macOS keys the per-app data directory off the bundle id:
#     ~/Library/Application Support/<CFBundleIdentifier>/
# OpenTerminal stores the AES-256-GCM-encrypted broker credential DB there, and
# the encryption key lives in the macOS Keychain. If the bundle id ever changes
# (a rename, a typo, a CMake edit, a tool mangling the plist), the next build
# reads a DIFFERENT data directory — so every user's saved broker credentials
# silently "vanish" and must be re-entered. This guard refuses to ship a build
# whose id drifted from the frozen value.
#
# Usage: verify-bundle-id.sh <path-to-.app>
# Exit:  0 = id matches; 1 = mismatch / missing plist (release should abort).
set -euo pipefail

# The frozen identity. Changing this is a deliberate, breaking act: update it
# here AND ship a migration that copies the old data dir to the new one.
EXPECTED="org.openterminal.OpenTerminal"

APP="${1:?usage: verify-bundle-id.sh <path-to-.app>}"
PLIST="$APP/Contents/Info.plist"

[ -f "$PLIST" ] || { echo "verify-bundle-id: no Info.plist at $PLIST" >&2; exit 1; }
ACTUAL="$(/usr/bin/plutil -extract CFBundleIdentifier raw "$PLIST" 2>/dev/null || true)"

if [ "$ACTUAL" != "$EXPECTED" ]; then
  {
    echo "=================================================================="
    echo "BUNDLE ID GUARD FAILED — refusing to sign/ship"
    echo "  expected: $EXPECTED"
    echo "  actual:   ${ACTUAL:-<empty>}"
    echo
    echo "CFBundleIdentifier drives the macOS data directory:"
    echo "  ~/Library/Application Support/<id>/"
    echo "which holds every user's encrypted broker credentials. Shipping a"
    echo "changed id orphans those credentials (users must re-enter them)."
    echo
    echo "If this rename is truly intended: update EXPECTED in this script AND"
    echo "ship a one-time migration that copies the old data dir to the new id."
    echo "=================================================================="
  } >&2
  exit 1
fi

echo "bundle-id guard OK: $ACTUAL"
