#!/usr/bin/env bash
# test_restart_kalshi_bot.sh — the build hook must never hijack a live trading job.
#
# scripts/deploy/restart-kalshi-bot.sh runs POST_BUILD on openterminalcli and used
# to rewrite the LaunchAgent's ProgramArguments[0] to whatever binary had just been
# built, then kickstart it. So a build from ANY tree — a throwaway git worktree, a
# feature branch, a half-finished checkout — silently repointed and restarted the
# operator's paper-trading bot. Observed twice in one session, once at a /tmp
# worktree that was then deleted, which would have left the agent pointing at a
# path that no longer existed.
#
# The rule this pins: the script may refresh the bot only when it is rebuilding the
# SAME binary the agent already runs. Any other path is refused, loudly, without
# touching the plist.
set -uo pipefail

SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/scripts/deploy/restart-kalshi-bot.sh"
[ -x "$SCRIPT" ] || { echo "FAIL: no executable script at $SCRIPT"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export HOME="$TMP"
mkdir -p "$HOME/Library/LaunchAgents"
PLIST="$HOME/Library/LaunchAgents/org.openterminal.kalshi-bot.plist"

write_plist() {
  cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>org.openterminal.kalshi-bot</string>
  <key>ProgramArguments</key><array><string>$1</string><string>kalshi</string><string>bot</string></array>
</dict></plist>
EOF
}

CANONICAL="$TMP/canonical/openterminalcli"
STRANGER="$TMP/stranger/openterminalcli"
mkdir -p "$(dirname "$CANONICAL")" "$(dirname "$STRANGER")"
printf '#!/bin/sh\nexit 0\n' > "$CANONICAL"; chmod +x "$CANONICAL"
printf '#!/bin/sh\nexit 0\n' > "$STRANGER"; chmod +x "$STRANGER"

# --- 1. a build from a DIFFERENT tree must not rewrite the plist -------------
write_plist "$CANONICAL"
OUT="$("$SCRIPT" "$STRANGER" 2>&1)"
RC=$?
[ "$RC" -eq 0 ] || fail "must stay non-fatal on refusal, got rc=$RC"
NOW="$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' "$PLIST" 2>/dev/null)"
[ "$NOW" = "$CANONICAL" ] || fail "plist was hijacked: expected '$CANONICAL', got '$NOW'"
echo "$OUT" | grep -qi "refus" || fail "refusal must be explained, got: $OUT"
echo "ok: a build from another tree leaves the live agent's binary alone"

# --- 2. the refusal must name both paths so the operator can act -------------
echo "$OUT" | grep -qF "$STRANGER" || fail "refusal must name the built binary"
echo "$OUT" | grep -qF "$CANONICAL" || fail "refusal must name the agent's current binary"
echo "ok: the refusal names what it built and what the agent runs"

# --- 3. a missing plist is not an invitation to create one ------------------
rm -f "$PLIST"
"$SCRIPT" "$STRANGER" >/dev/null 2>&1
[ ! -f "$PLIST" ] || fail "script created a LaunchAgent plist that did not exist"
echo "ok: no plist is created when none exists"

echo "PASS"
