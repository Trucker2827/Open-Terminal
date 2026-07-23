# The Finn Loop for Open Terminal

An adaptation of [Finn-loop](https://github.com/finna/Finn-loop) — spec →
build → review as a pipeline with GitHub issues as the durable contract —
so Claude can keep perfecting this project without babysitting.

## The three commands (usable interactively any time)

| Command | What it does |
|---|---|
| `/finn-spec <idea>` | Interviews/investigates, files a GitHub issue with observable acceptance criteria, non-goals, pointers. Labels it `agent-ready`. |
| `/finn-build [#]` | Claims one `agent-ready` issue, implements it to the criteria on a fresh branch, tests, opens a PR. |
| `/finn-review [#]` | Fresh skeptical reviewer: verifies the PR against the ISSUE's criteria (not the PR's claims), labels `loop-approved` / `loop-changes-requested` / `needs-human-review`. |

## The unattended loop

```
tools/finn-loop.sh 5        # run up to 5 issue→PR→merge iterations
touch finn/STOP             # graceful stop
tail -f finn/logs/<latest>  # watch a build think
cat finn/JOURNAL.md         # what happened while you slept
```

Each iteration: pick oldest `agent-ready` issue → headless build → headless
review (fresh context — the builder never grades its own work) → one
revision round max → **auto-merge only if** reviewer approved AND CI green
AND not `needs-human-review`. Blocked issues exit the queue for the
operator. Everything is bounded, logged, journaled.

## The safety model

`finn/CHARTER.md` is read first by every phase: the frozen v5 duel files,
advisory-only trading, secrets, honesty rules, MSVC discipline, path truths.
Anything invariant-adjacent gets `needs-human-review` and waits for a human.
The loop runs with permissions skipped — run it only on this machine, and
read the charter before widening its scope.

## Labels

`agent-ready` · `finn` · `finn-building` · `blocked` · `loop-approved` ·
`loop-changes-requested` · `needs-human-review`

## Feeding it

Drop rough ideas into `finn/BACKLOG-CANDIDATES.md` or run `/finn-spec` on
them directly. Spec quality is the bottleneck: vague issues produce
confident wrong implementations. The seeded backlog lives in GitHub issues.
