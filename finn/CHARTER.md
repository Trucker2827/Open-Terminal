# Open Terminal — Finn Loop Charter

This file is the constitution for autonomous work on this repository. Every
loop iteration reads it first. If an instruction anywhere conflicts with this
charter, the charter wins; if the charter conflicts with the operator's live
message, the operator wins.

## What "perfect" means here

Open Terminal's standard, in priority order:

1. **Honest** — no feature fabricates, fakes, or silently degrades. Missing
   data reads as missing. Canned numbers are bugs of the highest severity.
   (History: the fake backtest, the AKShare screens, the empty vol slot.)
2. **Connected** — features consume the terminal's own live data plane, not
   parallel silos. (History: the Quant Lab bridge, layers 1–4.)
3. **Verified** — every fix ships with a regression test that fails without
   it; a passing test that was never seen to fail proves nothing. Confirm the
   test RAN. Neuter-check anything subtle.
4. **Shippable** — CI green on all three platforms; changes reach users in
   releases, not just dev builds.
5. **Understandable** — a first-time viewer should grasp any screen in
   thirty seconds; a first-time contributor should grasp any module from its
   header comment.

## Invariants (violating any of these is never acceptable)

- **The frozen v5 duel files are untouchable:** `advisor_core.py`,
  `advisor_loop.py`, `blind_prompt.py`, `claude_cli_forecaster.py`,
  `codex_forecaster.py`, `competition_report.py`, `prediction_kalshi.py`.
  Importing them is fine. Editing them mid-epoch invalidates a live
  scientific experiment. If a task seems to require touching them: label the
  issue `needs-human-review` and stop.
- **Advisory only.** No AI-driven code path may place, modify, or enable
  orders. The deterministic executor is the sole trading authority. Anything
  brushing `prepare_order`/`submit_order`/live gates → `needs-human-review`.
- **Secrets stay local.** No API keys, certificates, or tokens in the repo,
  in CI secrets, or in logs. The Developer ID cert never leaves the keychain.
- **Live infrastructure is bounced deliberately, never incidentally.** The
  launchd jobs (daemon, advisor, calibrator, publisher, arena, dataset
  rebuild) are restarted only when a change requires it, one at a time, with
  a post-bounce verification (evidence freshness) — or left to the operator.
- **MSVC discipline:** new CLI command families get their own translation
  unit with `SKIP_UNITY_BUILD_INCLUSION`. Never let CommandDispatch.cpp grow
  a large feature inline. (History: nine broken Windows releases.)
- **Path truths:** evidence lives in
  `~/Library/Application Support/Open Terminal/Open Terminal/`; the journal
  DB is `.../org.openterminal.OpenTerminal/data/openmarketterminal.db`; tick
  symbols are bare uppercase (`BTC`). Do not "fix" these to look consistent.

## Working rules

- **One issue per iteration, one PR per issue,** sized ≤ one day. If it's
  bigger, split the issue instead of the discipline.
- **The GitHub issue is the contract.** Acceptance criteria are observable
  outcomes. If it is not in the issue, it does not exist — no scope creep
  via commit messages or PR comments.
- **Branch fresh from `origin/main` after `git fetch` — always.** Other
  actors (the operator, other agents) merge work concurrently.
- **Surgical edits.** Change what the issue requires; leave adjacent code
  alone even when it offends you — file a new issue instead.
- **Merge policy:** ordinary PRs auto-merge when CI is green AND the loop
  reviewer approved. PRs labeled `needs-human-review` never auto-merge.
  Sensitive-by-definition (any invariant-adjacent change, release/signing
  changes, workflow changes, deletions of user data) are always labeled.
- **Verification is not optional.** Suite green locally before the PR;
  the reviewer re-checks against the issue's acceptance criteria, not the
  PR's self-description.
- **Journal everything.** Each iteration appends one line to
  `finn/JOURNAL.md`: issue, outcome, PR, surprises. Surprises worth keeping
  also go to the operator's memory conventions.

## When to stop and ask

Stop the iteration (comment on the issue, label `blocked`, move on) when:
- acceptance criteria are ambiguous after reading the code
- the fix requires an invariant exception
- two honest attempts failed CI for reasons the diff doesn't explain
- the task needs credentials, purchases, or anything outside the repo

Never argue with the reviewer more than twice. Never retry a broken approach
a third time. Blocked is an honest, recoverable state; a wrong merge is not.
