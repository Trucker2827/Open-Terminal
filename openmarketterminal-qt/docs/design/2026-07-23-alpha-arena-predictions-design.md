# Alpha Arena: Predictions — design

*Open Terminal — 2026-07-23. Status: SPEC (phases at end).*

## One-paragraph pitch

Nof1's Alpha Arena made "AI models trading real money" a spectator sport, but
every arena in the 2026 landscape runs directional trading — and directional
PnL over a few weeks cannot separate skill from luck. Open Terminal already
runs the stricter thing for two models (the Claude-vs-Codex duel). **Alpha
Arena: Predictions generalizes that duel into an N-model arena on Kalshi
hourly contracts**: every model gets the identical price-free packet at the
same instant, commits a sealed probability before market odds are revealed,
and is scored by Brier against objective settlements — hundreds of events a
week, a leaderboard with real statistics, and a UI a first-time viewer can
understand in thirty seconds.

## Hard constraints (non-negotiable)

1. **The live v5 duel is untouched.** Its seven scoring files are
   hash-frozen; the arena is a parallel system with its own loop, report,
   and epochs. Shared C++ (challenge repository, blind packet allowlist) is
   reused read-only-in-behavior; any arena-driven change to shared C++ ships
   before new epochs only.
2. **Advisory-only, shadow-only.** Arena forecasters never touch order
   placement. The deterministic executor remains the sole trading authority.
3. **No fabricated anything.** Missing data is omitted; models that fail to
   answer are counted as non-coverage, never backfilled.

## Contestants: the forecaster registry

A contestant is a JSON entry, not code (`arena/forecasters.json`):

```json
{
  "id": "qwen3.5-local",
  "kind": "ollama",                    // ollama | openai_compat | cli
  "model": "qwen3.5:latest",
  "endpoint": "http://localhost:11434",
  "epoch_id": "arena-v1-qwen3.5",
  "timeout_s": 88,
  "enabled": true
}
```

- `ollama` / `openai_compat`: one generic adapter script
  (`arena_llm_forecaster.py`) drives any chat endpoint with the canonical
  instruction + blind packet, parses `{probability, confidence, rationale}`
  (JSON-fenced; the narrated-JSON fallback parser applies).
- `cli`: wraps the existing pinned claude/codex forecaster scripts unchanged
  — the duel contestants can guest-star in the arena under **new** epoch ids
  without their duel files changing.
- Local models make the arena cheap to run 24/7; cloud models join when keys
  exist. Version pinning per contestant; drift → that lane abstains with
  `INVALID_EPOCH` semantics scoped to the lane, not the whole arena.

## The round (grouped, not paired)

The duel's "paired opportunity" generalizes to a **group**:

```
picker (arena_loop.py) selects a fresh contract (>15 min runway, fresh evidence)
  → ONE immutable blind snapshot   (context_json, context_hash)
  → N sibling challenges           (same group_id, same context_hash, one per enabled lane)
  → all lanes run IN PARALLEL      (identical prompt bytes; per-lane symmetric timeout)
  → each lane commits blind        (sealed probability; market odds withheld)
  → settlement resolves            (outcome backfilled to every lane)
```

Fairness invariants, inherited from v5 and enforced per row:
- identical `prompt_hash` and `context_hash` across all lanes of a group
- commit-blind sealing (`sealed_hash` binds context + withheld market + nonce)
- symmetric latency budgets (same numbers for every lane)
- price-free allowlist packet (now including `realized_volatility` — every
  round has an objective difficulty: required move in sigmas)

## Scoring and the leaderboard (preregistered)

Per contestant, over jointly-offered groups:
- **Brier score** on resolved commitments (primary), with bootstrap CI
- **Coverage** = committed / offered (the v4 lesson: timeouts select easy
  cases; sub-80% coverage flags the lane's score as NOT COMPARABLE)
- **Calibration curve** (predicted vs realized frequency, 10 bins)
- **Difficulty split**: Brier on hard (required move < 1σ) vs easy rounds
- Rank by Brier among lanes with ≥ threshold coverage; a lane below coverage
  shows its number greyed with the reason — never silently dropped, never
  ranked.

Mechanical result states per season: `LEADER_DECLARED` (top lane's CI
excludes #2), `STATISTICAL_TIE`, `INSUFFICIENT_DATA`, `INVALID_EPOCH(lane)`.
Season = fixed window (e.g. 14 days) or fixed group count (e.g. 300), chosen
at open and immutable.

Integrity: `arena_report.py` journals a scoring-infrastructure SHA-256 per
group (manifest: arena loop, adapter, report, prompt builder, settlement
reader) — the duel's freeze mechanism, arena-scoped.

## Storage

Reuse `edge_advisory_challenge` (the repository already supports arbitrary
provider/model/epoch and a group id via `competition_pair_id` — rename-free:
the arena writes its group uuid there). Arena report reads the same table
filtered by arena epoch prefix. No schema migration needed for v1; a
`group_size` column is the only candidate for v2.

## UI: the Arena screen (the "easy to understand" part)

New screen `Alpha Arena` (sibling of Forecast Arena, reusing its widgets):

1. **Leaderboard** (the whole story at a glance):
   `# | Model | Brier ▼ | CI | Coverage | Resolved | Hard-case Brier | Trend`
   — green/grey rows by coverage validity; a one-line verdict banner
   ("No leader yet — 118/300 groups resolved").
2. **Live round strip**: the current contract (question, time left, difficulty
   in σ), and each lane's status dot (committed / thinking / timed out) —
   *without* revealing probabilities until the round's reveal time.
3. **Calibration panel**: per-model reliability curve — the visual that makes
   "well-calibrated" instantly meaningful to a non-statistician.
4. **How-it-works card** (plain language, always visible on first open):
   "Every model sees the same facts, never the market's odds. Each seals its
   probability before anyone can peek. Kalshi settles the answer within the
   hour. Lower Brier = better forecaster. Coverage below 80% means a model
   skipped the hard ones — its score doesn't count."
5. **Integrity drawer**: epoch ids, prompt/context hash spot-checks, scoring
   hash, per-lane version pins — the auditability that distinguishes this
   from every other arena.

CLI mirror: `openterminalcli arena status|leaderboard|round` for headless use,
and the MCP tools follow free (same pattern as existing screens).

## What this adds over Nof1's Alpha Arena (the venue thesis, operational)

objective hourly settlement · proper scoring rule · significance in weeks ·
forecasting isolated from sizing · intrinsic difficulty cohorts · full
methodology in-repo with hashes · anyone's local model can enter (the
"Open Arena" nobody else offers for predictions).

## Phases

- **P1 — Engine (headless arena, no UI):** forecaster registry +
  `arena_llm_forecaster.py` (ollama/openai-compat adapter) + `arena_loop.py`
  (group open → parallel lanes → commit-blind, reusing the challenge
  repository via the CLI like the duel does) + `arena_report.py`
  (leaderboard JSON with all gates) + launchd loop + tests. Contestants at
  launch: qwen3.5-local, llama3.1-local (+ duel CLIs optional, new epochs).
- **P2 — Arena screen:** leaderboard/live-round/calibration/how-it-works,
  reading `arena-report.json` evidence (same read-only evidence pattern as
  Forecast Arena).
- **P3 — Season mechanics & polish:** season open/close command with
  preregistered parameters, scoring-hash journaling, difficulty cohorts in
  UI, CLI/MCP surface, public read-only export (a shareable HTML page of the
  leaderboard).

Each phase is independently shippable; P1 alone produces a real leaderboard
in the report JSON within a day of contracts.
