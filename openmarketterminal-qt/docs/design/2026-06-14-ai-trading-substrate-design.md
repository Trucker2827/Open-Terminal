# AI-Trading Safety Substrate — Design Spec (Paper-First)

**Status:** Approved design (pending user spec review) — precedes the plan.
**Date:** 2026-06-14
**Builds on:** the shipped daemon + gates + `>=Verified` floor + off-thread dispatch; `UnifiedTrading`/`PaperTrading` (equity paper rail), `PositionManager` (risk floor), the prediction adapters (Polymarket/Kalshi).

## Goal

A safety substrate that lets an AI agent (Claude/Codex) **propose** trades while the **daemon is the final authority** that validates and executes or rejects — through a two-phase `prepare_order` → `submit_order` flow, with a deterministic risk floor the AI cannot bypass, full audit, and a **paper-first** posture where live execution is hard-off until paper results justify arming it.

**The governing rule:** the AI can *propose*; the CLI can *transmit*; only the **daemon** can *approve and execute*. The AI must never be able to arm or bypass its own trading.

## Two-layer constitutional model (the design philosophy)

The substrate is **not** "restrict the AI at every step" — that would make automation pointless. It is a **constitution**: a human sets the rules of the game once, then the AI plays freely inside them. Two layers, cleanly separated:

- **Layer 1 — Human authority (the arena walls).** A human, only in the GUI, sets the *constitutional controls*: whether AI live trading is on, max daily loss, max order size, max exposure per market/topic, the allowed account, allowed venues, allowed strategy universe, and the kill switch. The AI can **read** these but can **never change** them. These are the walls; the AI cannot move them.
- **Layer 2 — AI autonomy (play inside the arena).** Once the human enables AI trading, the AI operates **without per-trade human confirmation**: it prepares and submits orders, cancels/replaces, rebalances, exits positions, selects among the allowed markets, sizes within the caps, and runs continuously over the daemon/CLI. No random friction — that is the whole point of the automation.

**Human-owned controls (GUI-only — the AI cannot write any of these):**
- Enable/disable AI live trading (`cli.allow_trading`, `cli.live_trading_armed`)
- Enable AI paper trading (`cli.allow_paper_trading`)
- Max daily loss (`cli.risk.max_daily_loss`)
- Max order size (`cli.risk.max_order_value`), max position size (`cli.risk.max_position_qty`)
- Max exposure per market/topic (`cli.risk.max_exposure_*` — Phase B)
- Allowed account (`cli.allowed_account` — Phase C)
- Allowed venues (`cli.allowed_venues` — Phase B/C)
- Allowed strategy universe (`cli.allowed_strategies` — Phase C)
- Kill switch (`cli.kill_switch` — Phase C; always available, always honored)
- The settings-write grant itself (`cli.allow_settings_write`)

**AI-owned actions (free once the human has enabled the relevant mode):**
- Prepare orders; submit orders (paper now, live when armed) — **no per-trade confirmation**
- Cancel / replace orders; rebalance; exit positions (Phase C action set)
- Select among the *allowed* markets; size trades *within* the caps
- Run continuously through the daemon/CLI; log reasoning and results (audited)

**Why this is already enforceable for free:** the keystone is implemented as a single rule — `is_gui_only_setting(key) == key.startsWith("cli.")` — so the **entire `cli.*` namespace is constitutional**. Every control above is GUI-only the moment it is named under `cli.*`; adding a new wall (exposure cap, allowed-venues list, kill switch) needs **no change to the lock**. The AI can never raise its own limits, switch paper→live, disable the kill switch, or change the account/venues/strategy universe, because all of those are `cli.*` writes the `set_setting` tool refuses. Everything *inside* the walls is the AI's to do, freely.

## Phasing (per user)

- **Phase A — Equity paper MVP (this spec's buildable scope):** `prepare_order` + `submit_order` on the proven `UnifiedTrading` equity paper rail; `order_drafts` table; `trade_audit` table; deterministic risk floor at prepare AND submit; `cli.allow_paper_trading` toggle (default off); **live hard-off** (`submit_order mode=live` → "live disabled").
- **Phase B — Prediction-market paper:** the same substrate, new venue: `market_id`/`contract`(YES/NO)/`side`, probability-edge metadata, max-loss-from-contracts math, PM-specific risk (per-topic cap, liquidity/spread, settlement-proximity), wired to the Polymarket/Kalshi adapters in a paper/sim mode.
- **Phase C — Human-enabled AI live mode:** only after the paper path is proven. A human arms live in the GUI (`cli.allow_trading` + `cli.live_trading_armed`). Then the AI **submits live orders automatically, with NO per-trade human confirmation** — real automation, not a toy. The daemon still enforces the fixed, human-set constitutional limits (max daily loss / order size / exposure / allowed account+venues+strategies) re-checked fresh at every submit; the **AI cannot raise any of them**; the **kill switch is always available and always honored**; everything is audited. This phase also extends the AI action set beyond submit (cancel/replace, rebalance, exit) — those become AI-owned over the daemon *only when live is armed*, gated the same way the `submit_order` carve-out is (the action reaches its handler; the handler enforces the live + risk + allowed-universe constitution). (Coinbase PM adapter, if pursued, is its own venue add — availability uncertain.)

This document specifies the **full substrate architecture** but scopes the implementation **plan to Phase A**. The security properties (live hard-off, AI-can't-arm, risk floor, audit) are designed into Phase A even though live executes only in Phase C — the substrate must be safe from the first commit.

### Non-goals (this spec)
- Live execution enablement is **designed but not activated** (Phase C). Phase A ships live hard-off.
- PM venues / Coinbase adapter / PM-specific risk = Phase B+.
- The AI strategy logic / `openterminalcli ai run strategy …` loop — separate; this substrate is what such a loop would call.
- Any claim that the AI has edge — unproven; paper-first exists to measure it.

## Architecture (Phase A)

### Two-phase order flow
1. **`prepare_order(intent)`** — a *dry run + draft*, **non-destructive**:
   - Parse the structured intent into a `UnifiedOrder` (+ strategy/reason metadata).
   - Validate: instrument/symbol valid, account exists + allowed, order-type allowed, market sanity, and the **deterministic risk floor** (`PositionManager`: max order value, max position size, daily-loss headroom, exposure).
   - On pass: persist a draft to `order_drafts` (status `prepared`, short `expires_at`), return `{status:"prepared", draft_id, risk_status:"passed", max_loss, checks:[...]}`.
   - On fail: return `{status:"rejected", reason, checks:[...]}` — **no draft executes**.
   - Records a `trade_audit` row (phase `prepare`).
2. **`submit_order(draft_id, mode)`** — *execute*, **destructive**:
   - Load the draft (must exist, `prepared`, not expired).
   - **RE-RUN** the full validation against *fresh* market/account/risk state (the draft's verdict is stale; prices/exposure move between prepare and submit). A failed re-check → `{status:"rejected", reason}`.
   - **Gate by mode** (the ladder below).
   - Execute: `mode=paper` → `UnifiedTrading::place_order(paper_account, order)`; `mode=live` → Phase C (hard-off in A/B).
   - Update the draft (`submitted`/`filled`/`rejected`); record a `trade_audit` row (phase `submit`, with the decision + fill/rejection + risk snapshot).

### Structured intent (what the AI sends)
A strict JSON object (no free-text orders), e.g.:
```json
{ "symbol":"AAPL", "side":"buy", "quantity":10, "order_type":"limit",
  "limit_price":212.50, "account":"paper-default",
  "strategy":"momentum_breakout", "reason":"crossed configured breakout; exposure under limit" }
```
`strategy`/`reason` are recorded in the audit (the AI's thesis), never trusted for control. (Phase B adds `market_id`/`contract`/`estimated_probability`.)

### The gate ladder (who may do what)
| Action | Gate |
|---|---|
| `prepare_order` (validate + draft, no execution) | allowed (non-destructive dry-run; the AI needs it to plan) |
| `submit_order mode=paper` | **`cli.allow_paper_trading`** (default **off**) |
| `submit_order mode=live` | **`cli.allow_trading` AND `cli.live_trading_armed`** + risk floor + re-check — **Phase C only**; Phase A/B return `{status:"rejected","reason":"live trading disabled"}` |
| any other destructive tool over the daemon | denied (unchanged) |

### Daemon auth-checker carve-out
The daemon is read-only today (denies all `is_destructive`). `submit_order` is destructive but is the **one** carve-out: the daemon's auth-checker inspects `submit_order`'s `args.mode` and allows **paper** iff `cli_paper_trading_allowed()`, allows **live** iff `cli_trading_allowed() && cli_live_armed()` (Phase C), denies everything else destructive. The carve-out is explicit, narrow, and audited. (`prepare_order` is non-destructive → always reaches its own internal validation.)

### THE keystone safety property — the AI cannot arm its own trading
`cli.allow_trading`, `cli.live_trading_armed`, and `cli.allow_paper_trading` are **GUI-toggle-only**: they are added to a hard denylist in the settings-WRITE path, so **even with `cli.allow_settings_write` ON, `set_setting` refuses to change them** (returns an error). The CLI/AI can *read* them; only a human in the GUI Settings screen can change them. So a prompt-injected or compromised agent can never enable or arm its own trading. This is non-negotiable and is part of Phase A.

### Revocable live-enablement (supersedes the 2a cached-token idea)
There is **no cached destructive bearer token** on disk. `submit_order` re-reads the live settings (`cli.allow_trading`, `cli.live_trading_armed`) **at execution time, every submit**. Toggling trading off in the GUI takes effect on the very next submit — no replay window. The live *setting*, checked live, is the authority. (This cleanly resolves the 2a `bridge.json` destructive-token replay finding by removing the token entirely.)

### Deterministic risk floor (the daemon, not the AI)
At BOTH prepare and submit, `PositionManager`/the risk engine enforces: max order value, max position size, max daily loss / kill-switch, exposure caps — deterministic C++ outside any AI's control. A failing check rejects with a structured reason. The AI's `quantity`/`limit_price` are inputs to these checks, never overrides. (Phase B adds PM-specific caps.)

### Storage
- **`order_drafts`** (new table): `draft_id` (uuid), `intent_json`, `risk_verdict_json`, `account`, `mode_hint`, `status` (`prepared|submitted|rejected|expired`), `created_at`, `expires_at`. Drafts expire (e.g. 5 min) so a stale draft can't be submitted late; submit re-checks regardless.
- **`trade_audit`** (new, append-only): `id`, `ts`, `phase` (`prepare|submit`), `tool`, `account`, `mode`, `intent_json`, `decision` (`prepared|rejected|filled|denied`), `reason`, `risk_snapshot_json`. Every prepare, submit, rejection, and fill is recorded. (Dedicated table — richer than the PIN-oriented `SecurityAuditLog`.)

### MCP tools (Phase A)
- `prepare_order` (category `trading`, **non-destructive**), `submit_order` (category `trading`, **destructive** — the carve-out), `list_drafts` / `cancel_draft` (read/light). All flow through the daemon; submit is gated + audited.

## Data flow (one cycle)
AI reads state (`get_quote`, `portfolio_summary`) → forms a structured intent → `prepare_order` → daemon validates + risk-floor + drafts → returns verdict → AI `submit_order(draft_id, "paper")` → daemon **re-checks** + gate (`cli.allow_paper_trading`) + executes on the paper rail + audits → returns fill/rejection. Live is the same path with the live gates, **disabled in Phase A**.

## Error handling
Structured rejections everywhere: `{status:"rejected", reason, checks?}` (exit/`success:false` at the tool layer). Distinct reasons: gate-off ("paper trading disabled — enable in GUI Settings"), risk ("exceeds max position size"), stale ("draft expired — re-prepare"), live-off ("live trading disabled"). Never a vague failure.

## Testing strategy (Phase A)
- **Happy path (paper):** prepare a valid intent → draft + `risk_status:passed`; submit `paper` with `cli.allow_paper_trading=on` → fills on the paper rail; audit rows for both phases.
- **Risk floor:** an oversized intent → `prepare` rejects (max position); a submit whose risk changed since prepare → `submit` re-check rejects (proves re-validation).
- **Gate ladder:** `submit paper` with `cli.allow_paper_trading=off` → denied; `submit live` → "live disabled" regardless of toggles (Phase A hard-off).
- **AI-can't-arm (keystone):** `set_setting cli.allow_trading true` (even with `cli.allow_settings_write=on`) → **refused**; same for `cli.live_trading_armed`/`cli.allow_paper_trading`.
- **Revocable:** toggle `cli.allow_paper_trading` off mid-session → the next `submit paper` is denied (live settings read, no cached grant).
- **Audit completeness:** every prepare/submit/reject/fill produces a `trade_audit` row.
- **Daemon path:** the carve-out works over the bridge (daemon allows `submit_order paper` when the toggle's on; denies it off; denies all other destructive); off-thread dispatch keeps the daemon responsive.
- **No-regression:** existing `pt_place_order`/`live_place_order` tools, GUI selftests, attach, headless one-shot all still pass.

## Risks
- **The carve-out is a destructive path over the bridge** — mitigated by: paper-only in Phase A, the GUI-only-arming keystone, the deterministic risk floor, full audit, and live hard-off. Live (Phase C) only after paper is proven.
- **AI overconfidence** — the substrate's entire point: the daemon constrains the AI; paper-first measures whether there's real edge before any live arming. (Cited research: AI agents lost money trading PMs.)
- **Re-check race** (state changes between submit's re-check and execution) — the window is small; the risk floor + paper-first bound the damage; Phase C live would tighten this.

## Follow-ups (out of Phase A scope)
- **Phase B:** PM venue (Polymarket/Kalshi paper), the `market_id`/`contract` intent shape, max-loss-from-contracts, per-topic/liquidity/spread/settlement risk checks. Adds the constitutional controls `cli.risk.max_exposure_*` (per market/topic) and `cli.allowed_venues` — both GUI-only for free under the `cli.` prefix.
- **Phase C — human-enabled AI live mode (real automation, no per-trade confirm):**
  - GUI arm step (human flips `cli.allow_trading` + `cli.live_trading_armed`); after that, the AI submits live with NO per-trade prompt.
  - New constitutional controls (all GUI-only via the `cli.` prefix): `cli.allowed_account`, `cli.allowed_strategies`, `cli.kill_switch` (always honored — when set, every submit/cancel/replace is refused regardless of other gates).
  - **Enforce `cli.risk.max_daily_loss`** as a real running-P&L check (the recorded-but-not-yet-enforced cap from Phase A) — requires a fills/P&L tally in `trade_audit`/positions.
  - Extend the AI action set beyond submit: `cancel_order`/`replace_order`/`rebalance`/`exit_position` become AI-owned over the daemon **only when live is armed**, each via the same "checker opens the door → handler is the final authority (live + risk + allowed-universe + kill-switch)" pattern as the `submit_order` carve-out. No per-action human confirmation.
  - Live re-check hardening + **atomic reserve-before-execute** (the Phase-A `reserve_for_submit` CAS already lands this for submit; apply the same to cancel/replace), isolate the AI paper/live session from the process-global `UnifiedTrading::session_`, canonicalize the action-name match in the daemon checker.
- The `openterminalcli ai run strategy <name> --mode paper|live` higher-level loop — runs continuously inside the constitution.
- Per-sync-handler timeout (daemon throughput hardening, from the off-thread debt) — relevant once submit runs real work.
- Coinbase PM adapter (availability uncertain).

## Open questions (non-blocking Phase A)
- Draft expiry window (proposed 5 min) — tune in the plan.
- Whether `prepare_order` should itself require `cli.allow_paper_trading` (it creates a draft but doesn't trade) — proposed: no (dry-run is safe + the AI needs it); revisit if draft spam is a concern.
