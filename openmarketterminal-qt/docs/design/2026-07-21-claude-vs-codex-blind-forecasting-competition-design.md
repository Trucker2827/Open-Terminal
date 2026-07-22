# Claude vs Codex — Blind Shadow Forecasting Competition (Design v2)

**Date:** 2026-07-21
**Status:** Design approved (brainstorm) + Codex review amendments integrated. Pending final user sign-off → implementation plan.
**Owner split:** Claude architects/reviews; Codex implements. (Both are contestants; neither ever gets execution authority regardless of who wins.)
**Scope class:** Additive to the SHADOW/measurement layer only. No safety-spine, canary-envelope, or execution changes.

> v2 changelog: integrated Codex's six review amendments — explicit model pinning (§1), atomic paired-open (§3.1), hardened Claude invocation with verified flags (§4), anti-strategic-abstention reporting + coverage floor (§5.2), byte-for-byte prompt freeze (§5.1), and mechanical result states (§6). Feed-health caveat corrected (§11). Model pinned: `claude-opus-4-8` @ `--effort medium`. **v2.1: added §13 Forecast Arena UI guardrails (read-only mirror; provisional≠winner; hypothetical-not-P&L; coverage/abstention prominent; INVALID_EPOCH suppresses leader; no "trade" language).**

---

## 1. Goal & compute pinning

Add a second firewalled LLM forecaster — **Claude, run via the `claude` CLI** — beside the existing **Codex** forecaster, competing head-to-head on **forecast accuracy** over identical blind contexts. Pure measurement: both produce probability/confidence/rationale/abstention; **neither has execution authority**; the deterministic engine stays the sole executor, unchanged.

**Both contestants must record their full, exact compute configuration in every forecast row** (auditable, frozen per epoch):
- **Claude:** exact full model identifier (**no alias, no fallback**), reasoning `--effort`, `claude` CLI version, request timeout, prompt schema version, `prompt_hash` (§5.1). **Frozen for this epoch: model `claude-opus-4-8`, `--effort medium`** (Codex confirms the exact CLI-accepted model string at implementation; if the CLI resolves it to a fuller id, record that resolved id — still no alias).
- **Codex:** provider, exact model id, **effective reasoning effort**, CLI version, timeout, prompt schema version, `prompt_hash`.

A model alias (e.g. "opus") is forbidden — it can silently re-point to a different underlying model and corrupt the frozen epoch. Freeze the exact model string with no fallback; a fallback firing → **abstain + INVALID_EPOCH** (§4, §6).

## 2. Non-goals (OUT of this build)

- No consensus/average lane, no agreement-gate, no combination policy.
- No wiring of any LLM probability into the deterministic gate/EV (that is G2-live, gated on G5+G6 — unchanged).
- No change to the Codex v3 epoch's forecasting behavior, the deterministic engine, the safety spine, or the canary path.
- **No tools for the forecaster** (§4). The "tools help prediction" thesis is deferred to a documented future experiment (§9): in a liquid short-horizon market open tools mostly reach the contract's own price — leakage that manufactures false qualification, not edge (empirically demonstrated: `codex exec` read the live price file).

## 3. Architecture

- **New script:** `scripts/kalshi_advise/claude_cli_forecaster.py`, implementing the *same* pluggable forecaster contract as `codex_forecaster.py`:
  - `identify` → `{provider:"anthropic-claude-cli", model:<exact>, prompt_version, effort, cli_version, timeout_ms, schema_version, agent_id}`.
  - `predict` (blind context JSON on stdin) → `{decision:"predict", probability, confidence, rationale}` **or** `{decision:"abstain", reason_code, confidence, rationale}`.
- **Backend:** `claude` CLI headless (`claude -p`), tool-less + sandboxed (§4). No `ANTHROPIC_API_KEY` — CLI subscription auth, exactly as `codex_forecaster.py` uses the Codex CLI.
- **Advisor wiring:** per opportunity, run **both** forecasters on the **same immutable blind snapshot in parallel** (§3.1), journal both, score each in its own frozen epoch.

### 3.1 Atomic paired-open (Codex amendment #2 — approach (a), hardened)

For each opportunity, one atomic paired operation:
1. Capture **one immutable blind snapshot** (single `advise open`-equivalent read; do **not** call `advise open` twice — market state and timestamps would drift).
2. **Atomically create two sibling challenges** sharing a unique **`competition_pair_id`**, storing **identical canonical `context_json` and `context_hash`**.
3. Each sibling challenge has its **own** forecaster identity, TTL, state, and commit slot (a single challenge is consumed on one commit and cannot safely accept two).
4. **Launch both forecasters concurrently only after both challenge rows exist.**
5. Each forecaster commits blind to its own challenge; pairing is by `competition_pair_id` (+ identical `context_hash`).

## 4. Firewall — Claude invocation (BLOCKING; mirror of Codex v3 zero-capability lockdown)

The `claude` CLI is agentic (Bash/Read/WebFetch/MCP/hooks/plugins). Any of those can read `kalshi-ws-books.json` (live price) → leakage. The Claude forecaster MUST be structurally blind. **All flags below are verified present in `claude` 2.1.217.**

**Invocation (empty temporary cwd, blind context on stdin):**
```
claude -p \
  --output-format json \
  --model claude-opus-4-8                # exact; no alias, NO fallback
  --effort medium \
  --system-prompt "<FROZEN canonical instruction>"   # REPLACES the default agentic prompt (not --append)
  --exclude-dynamic-system-prompt-sections \          # strip dynamic/agentic framing → determinism
  --tools "" \                                         # zero-tool ALLOWLIST (see note: a config tool may survive)
  --strict-mcp-config --mcp-config '{"mcpServers":{}}' \  # empty MCP, strict (note: '{}' is INVALID → error)
  --disable-slash-commands \
  --no-chrome \
  --no-session-persistence \
  --safe-mode \
  --permission-mode manual                            # NEVER bypassPermissions/acceptEdits; tools empty anyway
```
Notes: `--system-prompt` (replace), not `--append-system-prompt` (which keeps the agentic default). `--permission-mode dontAsk` is **not** a valid choice (choices: acceptEdits/auto/bypassPermissions/manual) → use `manual`. `--mcp-config '{}'` is **invalid** (errors "mcpServers: expected record") → use `'{"mcpServers":{}}'`. `--cd` is a **Codex** flag, not Claude's — set the empty cwd via the subprocess working directory (`subprocess.run(..., cwd=tempdir)`), not a flag. Structured output: parse+validate the JSON on our side (regex-tolerant fallback + clamp, like `cli_forecaster.py`), since the `claude` CLI's structured-output surface differs from Codex's `--output-schema`.

**Stray-tool caveat (verified empirically 2026-07-21):** `--tools ""` does **not** guarantee zero tools — a tool defined in the user's Claude Code config (e.g. an `advisor` tool) can still appear in the session's tool list. Empirically the model still returned `CANNOT_READ`/`CANNOT_FETCH` (no leakage), but the surface-hash pin must **enumerate the actual resolved tool list** (or add `--disallowedTools` for any residual) so a config-injected tool is caught by the lockdown, not silently trusted. The **network firewall probe must distinguish** the CLI's *required* Anthropic API traffic from *model-initiated* fetches — a naive process-level check false-positives (`network:REACHED`) and forces spurious `CAPABILITY_LOCKDOWN_FAILED` abstentions.

**Lockdown pin (allowlist, fail-closed).** Because `claude` lacks Codex's convenient `features list` inventory, pin the **complete relevant CLI surface**: exact `claude --version` (`2.1.217`) **and** a hash of the pinned invocation/flag set + resolved model. Any drift — version change, unexpected **tool event, hook, plugin, MCP server, model fallback**, or flag-surface change — MUST **abstain (`CAPABILITY_LOCKDOWN_FAILED`) and invalidate the epoch** (INVALID_EPOCH, §6). On upgrade: human re-reviews the CLI surface, re-pins, and opens a new epoch (`...-v2`).

**Empirical firewall gate (BLOCKING — epoch does not count a single forecast until green).** Verify, as was done for Codex, that the locked forecaster cannot exfiltrate the hidden price via **either** channel:
- **File read:** ask it to read `~/Library/Application Support/Open Terminal/Open Terminal/kalshi-ws-books.json` → must `CANNOT_READ`/abstain.
- **Network:** ask it to fetch an external URL / the Kalshi price → must fail/abstain.
Re-run both probes on any `claude` upgrade.

## 5. Fairness protocol

### 5.1 Byte-for-byte prompt freeze (Codex amendment #5)
"Same prompt" means: the **same canonical forecasting instruction bytes** AND the **same serialized blind-packet bytes** for both contestants. CLI-specific wrappers may differ **only** where strictly required for structured output. Store the exact **`prompt_hash`** (of instruction+packet) in every forecast record. Divergent `prompt_hash` between siblings of a `competition_pair_id` → the pair is invalid.

### 5.2 Anti-strategic-abstention & preregistered decision rule (Codex amendment #4)
Jointly-resolved pairs alone can reward a model that only predicts easy cases. The report MUST include:
- Opened opportunities; per-model **prediction / abstention / expiration / error rates**.
- **Joint paired coverage**; coverage broken down by **regime / direction / distance-to-strike**.
- **Paired Brier delta with bootstrap CI** (over jointly-resolved pairs).
- **Each model's Brier on its full independently-committed sample** (not just paired).
- The preregistered thresholds and the resulting **result state** (§6).

**Preregistered (frozen before the epoch opens):** declare a winner only with **≥200 jointly-resolved pairs**, **≥80% prediction coverage for BOTH** contestants, and a paired-Brier CI that **does not cross zero**. Otherwise no winner.

### 5.3 Epoch discipline
New frozen epoch `kalshi-blind-claude-cli-v5-latency-neutral`, paired only with `kalshi-blind-codex-v4-zero-capability-latency-neutral` and **never pooled** with earlier or Ollama epochs. Same blind packet (price-free allowlist + defense-in-depth `FORBIDDEN_KEYS`), same TTL/commit-blind timing guard, same abstention semantics, same scoring path (`advise score --forecaster-id`), same `kMinResolvedSample`/CI.

### 5.4 Scoring-infrastructure freeze
At every opportunity the loop journals one deterministic SHA-256 manifest over the runtime-resolved Python components that can change selection, forecasts, pairing, settlement, or scoring: `advisor_core.py`, `advisor_loop.py`, `blind_prompt.py`, both CLI forecasters, `competition_report.py`, and `prediction_kalshi.py`. Missing files fail closed before an opportunity can open. The report publishes the current hash and compares it with every active-epoch row. A missing or changed row hash forces `INVALID_EPOCH`; infrastructure changes therefore require a new epoch and explicit review by both contestants before collection resumes. The Arena displays the abbreviated scoring hash beside the firewall and prompt/context integrity state. The compiled C++ scoring/advise surface remains pinned by the reviewed build commit; journaling a binary build stamp is a documented follow-up.

## 6. Result states (mechanical — no discretionary judgment after seeing results) (Codex amendment #6)

The competition report resolves to exactly one:
- `CLAUDE_WINS` — ≥200 paired, ≥80% coverage both, paired-Brier CI favors Claude and excludes zero.
- `CODEX_WINS` — symmetric.
- `STATISTICAL_TIE` — thresholds met but CI crosses zero.
- `INSUFFICIENT_PAIRED_DATA` — <200 paired or <80% coverage for either.
- `INVALID_EPOCH` — any firewall breach, lockdown drift, model fallback, prompt_hash divergence, pairing corruption, or scoring-infrastructure drift during the epoch.

## 7. Data flow

```
opportunity → capture ONE immutable blind snapshot (context_json, context_hash)
   → atomically create sibling challenges (competition_pair_id, identical context)
   ├─ parallel → codex_forecaster.py      → {predict|abstain} → commit-blind → journal (epoch: kalshi-blind-codex-v4-zero-capability-latency-neutral)
   └─ parallel → claude_cli_forecaster.py → {predict|abstain} → commit-blind → journal (epoch: kalshi-blind-claude-cli-v5-latency-neutral)
   → resolve at settlement (outcome backfill)
   → advise score --forecaster-id  (each epoch independently)
   → competition report (§5.2) → result state (§6)
```

## 8. Testing / verification (all pass; firewall probes are hard gates)

- **Unit:** forecaster contract (predict/abstain schema; abstain requires reason_code, omits probability; malformed output → abstain); lockdown drift (bad version / flag-surface hash / model fallback) → `CAPABILITY_LOCKDOWN_FAILED` abstain.
- **Firewall probes (blocking):** file-read → `CANNOT_READ`; network → fail. Epoch doesn't accumulate until both green.
- **Integration:** one opportunity → exactly one Claude + one Codex challenge, same `competition_pair_id` + `context_hash` + `prompt_hash`, distinct epoch ids; slow forecast expires honestly.
- **Report:** coverage/abstention/rate fields populated; result state computed only from preregistered thresholds.
- **Discipline:** every claim confirmed by running the check; firewall probes + drift-abstention re-run on any `claude` upgrade.

## 9. Future experiment (documented, NOT built here): computation-tool epoch

Test "do tools help prediction?" *without leakage*: a separate later epoch gives **both** models exactly one sandboxed **computation tool** (probability calculator / small Monte-Carlo / historical base-rate lookup) with **no data, price, web, shell, file, or MCP access.** Blind-to-price is preserved, so Brier-vs-market stays valid. Measure whether the tool-equipped model beats its own blind self on the same paired contracts. Own frozen epoch; never pooled with the blind epochs.

## 10. Honesty note (self-evaluation)

Claude authored this and reviews results — but §6 makes the outcome **mechanical**: paired Brier + preregistered thresholds → a fixed result state, with **no discretionary judgment**. Claude's lane is held to the identical frozen gates as Codex's. If Claude's lane ever appears graded gently, that is a bug to flag.

## 11. Operational reality (corrected per Codex, 2026-07-21)

The **daemon Kalshi feed is currently healthy** (connected, 17 markets / 34 assets, ~ms-fresh books, 0 reconnects on the current daemon PID). The WS liveness-watchdog fix (PR #56) is verified. The advisor consumes the daemon-generated `kalshi-ws-books.json`; the GUI's stale selected-contract panel is a separate presentation/subscription issue and does not feed the competition. `NO_FRESH_CONTRACT` is an honest horizon/freshness abstention when no eligible daemon snapshot exists. The competition may accumulate shadow forecasts while canary promotion remains independently blocked by unresolved reconciliation and qualification gates.

## 12. Risks & mitigations

- **Leakage via tools/MCP/hooks/network** → structurally prevented (§4) + dual empirical probes (file + network); hard gate before the epoch counts.
- **Strategic abstention** → coverage floor (≥80% both) + full rate/coverage reporting (§5.2); no winner below threshold.
- **Silent model drift** (alias/fallback) → exact-model pin, no fallback → abstain + INVALID_EPOCH.
- **Unfair comparison** (prompt/timing/tier) → byte-for-byte prompt freeze (§5.1), atomic paired-open with identical context (§3.1), independent frozen epochs.
- **Timeout-budget bias** (measured 2026-07-21) → with `--forecast-timeout 35s`, `claude-opus-4-8 @ medium` (median 13.6s, heavy tail) timed out 12× — **all on 1h contracts with 30–57 min runway** — while `gpt-5.6-sol` (median 31.6s, tighter) never did, flooding `PAIRED_PARTIAL` and letting the tighter-latency model win timeout races by default (skill-irrelevant bias). Contract-horizon is NOT the lever (picker already `--auto-min-secs-left 901`). **Fix:** set `--forecast-timeout` generously (~90s) so **both** tails finish; ensure `elapsed ≤ prediction_ttl_ms − 6000` supports it (raise `ttl_for` `configured_max_ms=100000` for the competition epoch if it caps <~96s). The budget must cover both tails or the paired sample is biased.
- **CLI upgrade re-enabling tools** → version + flag-surface pin → abstain on drift; re-probe on upgrade.
- **Epoch pooling** → strict `forecaster-id`/`prompt_version` filtering; distinct epoch ids; INVALID_EPOCH on cross-contamination.

## 13. Forecast Arena UI — guardrails (Codex builds the Qt Notebook window; these are binding)

A dedicated Qt Notebook window, **"Forecast Arena" (CLAUDE vs CODEX · LIVE SHADOW DUEL)**, visualizes the competition. It is a **presentation layer only**; the following are implementation requirements, not suggestions.

**Language discipline (safety):** Never call a forecast a "trade," "order," or "position." Each row is a **blind forecast on an opportunity**. Persistent banners: **SHADOW ONLY · NO ORDERS · NO EXECUTION AUTHORITY**.

**Binding guardrails:**
1. **Read-only mirror, not a control surface or second source of truth.** The Arena renders exactly what the journal + `competition_report.py` emit — including the **same `result_state` from `compute_result_state` (never a UI re-derivation)**. It exposes **no** action that opens a challenge, triggers a forecast, enables canary, or mutates the epoch. Viewer only.
2. **Provisional ≠ winner.** Show the running paired-Brier delta + CI + a *provisional* leader, but the **WINNER banner may only render on `CLAUDE_WINS`/`CODEX_WINS`** from the preregistered gate (§5.2: ≥200 pairs, ≥80% coverage both, CI excludes zero). Below the gate, the provisional leader is styled explicitly **non-decisive** ("provisional · not yet significant").
3. **Hypothetical value is labeled counterfactual.** Any "net-of-fees value" chart is a **HYPOTHETICAL** scoring aid ("if these blind forecasts had been sized by a fixed rule…") — it must not render like a real equity curve or imply either model held a position.
4. **Coverage + abstention are prominent, not buried.** Each model's prediction/abstention/expiration/error rates and coverage-by-regime sit *beside* the score (surfacing §5.2 — "leads but abstained on the hard cases" must be visible at a glance, not hidden behind the delta).
5. **`INVALID_EPOCH` is loud, sticky, and suppresses any leader.** On firewall breach / lockdown drift / model fallback / `prompt_hash` divergence, the Arena shows a prominent INVALID banner and **refuses to display a provisional or final leader** on tainted data.
6. **Epoch-scoped, never pooled.** The window names the epoch pair it displays (`kalshi-blind-claude-cli-v5-latency-neutral` ↔ `kalshi-blind-codex-v4-zero-capability-latency-neutral`), never mixes epochs, and follows the frozen pair when a `-v2` opens after a `claude` upgrade.
7. **Firewall-integrity panel:** model/CLI/prompt versions, epoch ids, `prompt_hash`/`context_hash`, and live capability-lockdown status — so blindness is auditable in the UI.
8. **No dead ends:** each opportunity row is clickable → a comparison card with both rationales, the identical blind packet, blind-commit/reveal/outcome timestamps (UTC), post-reveal market info, the scoring arithmetic, and audit hashes.
9. **Result-state enum matches the report exactly:** `CLAUDE_WINS | CODEX_WINS | STATISTICAL_TIE | INSUFFICIENT_PAIRED_DATA | INVALID_EPOCH` (note: `INSUFFICIENT_PAIRED_DATA`, not `INSUFFICIENT_DATA`) — no UI-side translation layer.
10. **Honest pre-competition state** until paired data exists: **`ARENA INITIALIZING · 0/200 JOINTLY RESOLVED · NO WINNER YET`**.
11. **Local only:** no share/export/publish path that could leak account, position, or credential data — the Arena stays inside the app.

**Review gate:** the Arena PR is reviewed (Claude) for: read-only (no control actions), WINNER banner strictly gated on `compute_result_state`, "hypothetical" labeling, INVALID_EPOCH leader-suppression, coverage/abstention prominence, and zero "trade/order/position" language on any forecast.
