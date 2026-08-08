#!/usr/bin/env bash
# Best-effort: after rebuilding openterminalcli, kick the paper bot LaunchAgent
# so it loads the new binary without a manual reset.
#
# Usage: restart-kalshi-bot.sh [path-to-openterminalcli]
# Exit 0 even when the agent is not loaded (dev machines without launchd).

set -u

LABEL="org.openterminal.kalshi-bot"
CLI_PATH="${1:-}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  exit 0
fi

UID_NUM="$(id -u)"
DOMAIN="gui/${UID_NUM}"

if ! launchctl print "${DOMAIN}/${LABEL}" >/dev/null 2>&1; then
  echo "kalshi-bot: LaunchAgent ${LABEL} not loaded — skip restart"
  exit 0
fi

if [[ -n "${CLI_PATH}" && -x "${CLI_PATH}" ]]; then
  # Keep the plist's ProgramArguments[0] honest when the build tree moved.
  PLIST="${HOME}/Library/LaunchAgents/${LABEL}.plist"
  if [[ -f "${PLIST}" ]]; then
    /usr/libexec/PlistBuddy -c "Set :ProgramArguments:0 ${CLI_PATH}" "${PLIST}" 2>/dev/null || true
  fi
fi

if launchctl kickstart -k "${DOMAIN}/${LABEL}" >/dev/null 2>&1; then
  echo "kalshi-bot: kickstarted ${LABEL} → ${CLI_PATH:-plist binary}"
else
  echo "kalshi-bot: kickstart failed (non-fatal)"
fi
exit 0
