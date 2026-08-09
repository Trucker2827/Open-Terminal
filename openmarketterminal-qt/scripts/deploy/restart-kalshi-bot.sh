#!/usr/bin/env bash
# Best-effort: after rebuilding openterminalcli, kick the paper bot LaunchAgent
# so it loads the new binary without a manual reset.
#
# Usage: restart-kalshi-bot.sh [path-to-openterminalcli]
# Exit 0 even when the agent is not loaded (dev machines without launchd).
#
# THE RULE: this may refresh the bot only when it is rebuilding the SAME binary
# the agent already runs. It must never repoint a live trading job at a
# different tree.
#
# It used to. It rewrote ProgramArguments[0] to whatever binary had just been
# built and then kickstarted it, so a build from ANY tree -- a throwaway
# worktree, a feature branch, a half-finished checkout -- silently hijacked the
# operator's paper-trading bot. That fired twice in one session, once from a
# /tmp worktree that was then deleted, which would have left the agent pointing
# at a path that no longer existed. Pinned by tests/test_restart_kalshi_bot.sh.
set -u

LABEL="org.openterminal.kalshi-bot"
CLI_PATH="${1:-}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  exit 0
fi

PLIST="${HOME}/Library/LaunchAgents/${LABEL}.plist"

# No plist means nothing to refresh. Never create one -- installing a trading
# agent is a deliberate act, not a side effect of compiling.
if [[ ! -f "${PLIST}" ]]; then
  echo "kalshi-bot: no LaunchAgent plist at ${PLIST} — nothing to refresh"
  exit 0
fi

CURRENT="$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' "${PLIST}" 2>/dev/null || true)"

# The guard. A build of some other tree is not authority to move a live job.
if [[ -n "${CLI_PATH}" && -n "${CURRENT}" && "${CLI_PATH}" != "${CURRENT}" ]]; then
  echo "kalshi-bot: REFUSING to repoint the live agent."
  echo "  built:          ${CLI_PATH}"
  echo "  agent runs:     ${CURRENT}"
  echo "  A build of another tree must not move a running trading job. If you"
  echo "  really intend to switch the agent to the binary you just built, do it"
  echo "  deliberately:"
  echo "    /usr/libexec/PlistBuddy -c 'Set :ProgramArguments:0 ${CLI_PATH}' '${PLIST}'"
  echo "    launchctl kickstart -k gui/\$(id -u)/${LABEL}"
  exit 0
fi

UID_NUM="$(id -u)"
DOMAIN="gui/${UID_NUM}"

if ! launchctl print "${DOMAIN}/${LABEL}" >/dev/null 2>&1; then
  echo "kalshi-bot: LaunchAgent ${LABEL} not loaded — skip restart"
  exit 0
fi

# Same binary the agent already runs: refreshing it is safe and is the only
# case this script exists for.
if launchctl kickstart -k "${DOMAIN}/${LABEL}" >/dev/null 2>&1; then
  echo "kalshi-bot: kickstarted ${LABEL} → ${CURRENT:-plist binary}"
else
  echo "kalshi-bot: kickstart failed (non-fatal)"
fi
exit 0
