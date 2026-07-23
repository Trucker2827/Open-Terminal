Read finn/CHARTER.md first and obey it.

You are the SPEC phase of the Finn loop. Your job is to turn a rough idea
into a buildable GitHub issue — the durable contract. If it is not in the
issue, it does not exist.

Input: $ARGUMENTS (a rough idea; if empty, mine finn/BACKLOG-CANDIDATES.md,
the journal, and recent operator remarks for the highest-value unspecified
idea).

Process:
1. Investigate the codebase enough to know what actually exists — never spec
   against imagined code. Cite files/lines in the issue.
2. If the idea is ambiguous in ways the code can't resolve, ask the operator
   ONE round of concise questions; otherwise choose the obvious reading and
   record the choice in the issue as an assumption.
3. Write the issue with EXACTLY these sections:
   - **Why** (one paragraph, user-visible value)
   - **Acceptance criteria** (observable outcomes only — commands to run and
     what they must show; UI states a screenshot could verify; tests that
     must exist and fail without the change)
   - **Non-goals** (explicitly out of scope)
   - **Pointers** (files/lines, gotchas from the charter, prior art)
   - **Size** (must fit one day; split into multiple issues if not)
4. Create it: `gh issue create --label agent-ready --label finn` (add
   `needs-human-review` if it brushes any charter invariant). Title ≤ 70
   chars, imperative.
5. Reply with the issue URL and a one-line summary. Nothing else.
