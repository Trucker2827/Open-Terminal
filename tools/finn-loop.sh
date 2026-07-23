#!/bin/zsh
# finn-loop.sh — the unattended build→review→merge loop for Open Terminal.
#
# Adapted from Finn-loop (github.com/finna/Finn-loop) for this repository:
# GitHub issues are the durable contract (labels: agent-ready / finn-building
# / blocked / loop-approved / loop-changes-requested / needs-human-review),
# finn/CHARTER.md is the constitution, and — one deliberate deviation from
# upstream's "humans merge" rule, per operator preference — ordinary PRs
# auto-merge when the loop reviewer approved AND CI is green. PRs labeled
# needs-human-review NEVER auto-merge.
#
# Usage:
#   tools/finn-loop.sh [max_iterations]     # default 5
#   touch finn/STOP                          # graceful stop after current step
#
# Each iteration: one issue → build (fresh headless Claude) → review (fresh
# headless Claude) → merge-if-approved-and-green → journal. Bounded, logged,
# stoppable. Run it from the repo root on a machine where `claude` and `gh`
# are authenticated.

set -uo pipefail
cd "$(dirname "$0")/.."

MAX_ITER="${1:-5}"
LOG_DIR="finn/logs"
mkdir -p "$LOG_DIR"
STAMP() { date "+%Y-%m-%d %H:%M:%S" }

journal() {
  echo "- $(STAMP) $1" >> finn/JOURNAL.md
}

run_claude() { # run_claude <logfile> <prompt...>
  local log="$1"; shift
  # --dangerously-skip-permissions: required for unattended operation.
  # The charter + sensitive-label gates are the safety model; do not run
  # this loop on a machine whose repo or credentials you can't afford to
  # trust an agent with.
  claude -p "$*" --dangerously-skip-permissions >> "$log" 2>&1
}

# Builders may leave the checkout on their feature branch; return to fresh
# main between iterations. Never force: only the (tracked, always-dirty)
# journal is auto-resolved — any other local change aborts the switch.
ensure_main() {
  if [[ "$(git branch --show-current)" != "main" ]]; then
    cp finn/JOURNAL.md "$LOG_DIR/.journal.bak" 2>/dev/null
    git checkout -q -- finn/JOURNAL.md 2>/dev/null
    if git checkout -q main 2>/dev/null; then
      cp "$LOG_DIR/.journal.bak" finn/JOURNAL.md 2>/dev/null
    else
      cp "$LOG_DIR/.journal.bak" finn/JOURNAL.md 2>/dev/null
      journal "WARNING: cannot return checkout to main (dirty tree) — on $(git branch --show-current)"
      return 1
    fi
  fi
  git pull --ff-only -q 2>/dev/null || true
}

HANDLED=" "
for i in $(seq 1 "$MAX_ITER"); do
  if [[ -f finn/STOP ]]; then
    journal "loop stopped by finn/STOP before iteration $i"
    rm -f finn/STOP
    exit 0
  fi

  ensure_main

  # GitHub's list API is eventually consistent: an issue closed+delabeled
  # seconds ago can still appear here. One attempt per issue per run.
  # An empty result can also mean gh/network failure (seen 2026-07-23:
  # ENOTFOUND ended a run early) — retry before believing "no issues".
  CANDIDATES=""
  for attempt in 1 2 3; do
    CANDIDATES=$(gh issue list --label agent-ready --state open --json number,labels \
      --jq '[ .[] | select((.labels | map(.name) | index("blocked")) == null)
                   | select((.labels | map(.name) | index("finn-building")) == null) ]
             | sort_by(.number) | .[].number' 2>/dev/null)
    [[ -n "$CANDIDATES" ]] && break
    sleep 60
  done
  ISSUE=""
  for cand in ${(f)CANDIDATES}; do
    [[ "$HANDLED" == *" $cand "* ]] || { ISSUE="$cand"; break }
  done
  if [[ -z "$ISSUE" || "$ISSUE" == "null" ]]; then
    journal "no unhandled agent-ready issues; loop idle-exits at iteration $i"
    exit 0
  fi
  HANDLED="$HANDLED$ISSUE "

  LOG="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-issue-$ISSUE.log"
  journal "iteration $i: building issue #$ISSUE (log: $LOG)"

  run_claude "$LOG" "/finn-build $ISSUE"

  PR=$(gh pr list --label finn --state open --search "issue:$ISSUE in:body" \
        --json number --jq '.[0].number' 2>/dev/null)
  [[ -z "$PR" || "$PR" == "null" ]] && \
    PR=$(gh pr list --state open --search "\"#$ISSUE\" in:body" --json number \
          --jq '.[0].number' 2>/dev/null)
  if [[ -z "$PR" || "$PR" == "null" ]]; then
    journal "issue #$ISSUE: no PR after build (blocked or failed) — see log"
    continue
  fi

  run_claude "$LOG" "/finn-review $PR"

  LABELS=$(gh pr view "$PR" --json labels --jq '.labels[].name' | tr '\n' ' ')
  if [[ "$LABELS" == *needs-human-review* ]]; then
    journal "issue #$ISSUE PR #$PR: needs-human-review — left for operator"
    continue
  fi
  if [[ "$LABELS" != *loop-approved* ]]; then
    # one revision round: build gets the reviewer's comments, then re-review
    run_claude "$LOG" "/finn-build $ISSUE (address the loop reviewer's change requests on PR #$PR; push to the same branch)"
    run_claude "$LOG" "/finn-review $PR"
    LABELS=$(gh pr view "$PR" --json labels --jq '.labels[].name' | tr '\n' ' ')
  fi
  if [[ "$LABELS" != *loop-approved* ]]; then
    journal "issue #$ISSUE PR #$PR: not approved after revision — left open"
    continue
  fi

  # merge only when CI is fully green
  for wait in $(seq 1 45); do
    STATUS=$(gh pr checks "$PR" 2>/dev/null | awk '{print $2}' | sort -u | tr '\n' ' ')
    [[ "$STATUS" == *fail* ]] && break
    [[ "$STATUS" != *pending* && "$STATUS" == *pass* ]] && break
    sleep 60
  done
  if [[ "$STATUS" == *fail* || "$STATUS" == *pending* ]]; then
    journal "issue #$ISSUE PR #$PR: approved but CI not green ($STATUS) — left open"
    continue
  fi

  if gh pr merge "$PR" --merge >> "$LOG" 2>&1; then
    journal "issue #$ISSUE PR #$PR: MERGED"
    # Close + delabel immediately — GitHub's Closes-link fires async and the
    # next iteration's selection can otherwise race onto the same issue.
    gh issue close "$ISSUE" >> "$LOG" 2>&1 || true
    gh issue edit "$ISSUE" --remove-label agent-ready --remove-label finn-building \
      >> "$LOG" 2>&1 || true
  else
    journal "issue #$ISSUE PR #$PR: merge failed — see log"
  fi
done

ensure_main
journal "loop completed $MAX_ITER iterations"
