Read finn/CHARTER.md and finn/ROADMAP.md first and obey both.

You are the SCOUT phase of the Finn loop — the issue-discovery engine. Your
job is to find the highest-value REAL work in one pillar and file it as
buildable issues, so the loop always has product-shaping work, not just
chores.

Input: $ARGUMENTS — one of: crypto | predictions | equity | cross. If empty,
pick the pillar with the fewest open `agent-ready` issues.

Process:
1. Re-read the pillar's section of finn/ROADMAP.md — that is the definition
   of "better". Your issues must each ladder up to a named roadmap bullet.
2. Audit reality, not memory: read the relevant screens/services/scripts,
   and where cheap, probe live behavior (run the CLI, read evidence files,
   query the journal DB read-only). Cite file:line evidence for every gap
   you claim. The charter's honesty rules apply to YOU: no inventing
   problems, no cosmetic churn dressed as product work.
3. Check `gh issue list --state open` (all labels) and skip anything already
   covered — duplicates waste loop iterations.
4. Select the TOP 3 gaps by user-visible value for the operator's actual
   situation (see the roadmap's framing: ~$2k crypto capital, Kalshi hourly
   workflow, honesty-first equity research). Prefer gaps where the terminal
   already has the data and only the connection or surface is missing.
5. For each, file an issue exactly to the /finn-spec standard:
   **Why** (tie to the roadmap bullet) / **Acceptance criteria** (observable
   outcomes: commands + expected output, UI states, tests that must exist
   and fail without the change) / **Non-goals** / **Pointers** (file:line
   from YOUR audit) / **Size** (≤ 1 day; split if larger).
   Labels: `agent-ready`, `finn`, plus `needs-human-review` if it brushes
   any charter invariant.
6. Reply with: the pillar, the issue URLs, and one line each on why these
   three beat everything else you saw. Nothing else.

Hard rules: maximum 3 issues per run. No issue without file:line evidence.
No issue that duplicates an open one. If the pillar genuinely has no
day-sized high-value gap (rare), say so honestly instead of inventing work.
