# AI-Trading Substrate — Phase C (Human-Enabled AI Live Mode) Design Spec

**Status:** Draft design — pending user spec review. Precedes the plan. **Real-money phase — review required before any build.**
**Date:** 2026-06-15
**Builds on:** Phase A (equity paper) + Phase B (PM paper) — the two-phase `prepare_order`/`submit_order`, the deterministic risk floor, `order_drafts`/`trade_audit`, the GUI-only `cli.` constitution, the daemon/headless `submit_order` carve-out (which ALREADY allows `submit_order mode:"live"` through to the handler iff `cli_trading_allowed() && cli_live_armed()`), and the live rails: `UnifiedTrading::place_order(account_id, order)` (account-aware → real broker via `AccountManager`; Alpaca/Tradier/Hyperliquid) + the PM `PredictionExchangeAdapter::place_order`.
**Parent spec:** `2026-06-14-ai-trading-substrate-design.md` (the two-layer constitutional model).

## Goal

Turn on **human-enabled AI live mode**: once a human arms live in the GUI, the AI submits **real** orders automatically — **no per-trade confirmation** — on **both the equity and prediction-market rails**, while the daemon deterministically enforces the human-set constitution the AI cannot change (arm toggles, allowed account, risk caps, daily-loss limit, kill switch). This is Layer 2 of the constitution finally executing for real, with Layer 1 as the unbreakable arena walls.

**User decisions (this spec is built on these):**
- **Rails:** both equity (broker) and prediction-market live.
- **Validation:** **broker sandbox only** during development — the live path is built + tested against a broker SANDBOX account (real API, fake money; Alpaca paper API for equity, Kalshi demo where supported for PM). Real money is enabled solely by the human configuring a `live`-mode account with real credentials. **No real money is risked in development.**
- **Action set:** `submit_order mode:"live"` only. Cancel/replace/rebalance/exit-position stay paper-only / deferred to a follow-up (smallest real-money surface).

## The keystone realization: the account is the safety lever

The AI never chooses or configures the broker account. A human, in the GUI, configures accounts (broker, credentials, **mode** = `paper`/`sandbox`/`live`) and designates exactly one as AI-tradeable via **`cli.allowed_account`** (GUI-only). The AI submits against that account id; `UnifiedTrading` routes by the account's human-set mode:
- account mode `sandbox`/`demo` → the broker's SANDBOX endpoint (fake money) — the development + safe-operation target.
- account mode `live` → real money — only because a human configured real credentials AND set the mode AND allowed it AND armed live.
- account mode `paper` (or anything unrecognized) → the internal paper simulator (existing fail-safe: `UnifiedTrading.cpp` only takes the broker path for `live`/`sandbox`/`demo`).

So **"sandbox vs real money" is entirely a human GUI decision the AI cannot see or change.** The substrate's job is identical for both; the human flips the real-money switch by swapping the allowed account's credentials/mode. This is why the build is safe: every test runs against a sandbox account; real money is a human credential change, not a code path the AI controls.

## Two-layer constitution — the full Phase-C control set (all GUI-only via the `cli.` prefix)

The keystone (`is_gui_only_setting(key) == key.startsWith("cli.")`) makes every control below GUI-only for free — `set_setting` refuses them, so the AI can never arm itself, pick its own account, raise its caps, or disable the kill switch.

**Human-owned (Layer 1):**
- `cli.allow_trading` + `cli.live_trading_armed` — the two-key arm (BOTH required; default off).
- `cli.allowed_account` — the single account id the AI may trade (default empty = none).
- `cli.allowed_venues` (Phase B) — for PM live; equity uses the account's broker.
- `cli.kill_switch` — **NEW**: when `"true"`, ALL execution (paper AND live) is refused. Always honored, checked first.
- `cli.risk.*` caps (Phase A/B) — order value, position size, exposure-per-topic, PM liquidity/spread/proximity.
- `cli.risk.max_daily_loss` — **NEW enforcement**: a running realized-P&L tally; breaching it halts new orders.

**AI-owned (Layer 2), once armed:**
- `submit_order mode:"live"` — automatic, no per-trade confirmation, against `cli.allowed_account`, inside every cap.
- (Phase A/B) prepare/submit paper, the read tools, run continuously, audited.

## Architecture

### The arming flow (human, GUI)
1. Human configures a broker/PM account (credentials + mode) in Settings → designates it via `cli.allowed_account`.
2. Human sets `cli.allow_trading=true` AND `cli.live_trading_armed=true` (two distinct toggles — the "advanced, I mean it" double-arm).
3. Human ensures `cli.kill_switch=false` and the risk caps are set.
From then on the AI's `submit_order mode:"live"` executes against that account with no further human action — until the human revokes (any of: un-arm, engage kill switch, clear the allowed account) which takes effect on the very next submit (revocable, live-read).

### `submit_order mode:"live"` handler (the change from A/B hard-off)
Replace the live hard-off with a gated live execution path. At submit, in order (all re-read LIVE — revocable):
1. **Kill switch:** `cli.kill_switch` true → reject "kill switch engaged" (also applies to paper). Audited.
2. **Arm:** `cli_trading_allowed() && cli_live_armed()` → else reject "live trading not armed". (The daemon checker already enforces this before the handler; the handler re-checks — defense in depth + clean message.)
3. **Account:** resolve `cli.allowed_account`; the draft/intent's account (if any) must equal it; reject "account not allowed for AI trading" otherwise. Load the account; confirm it exists.
4. **Re-validate fresh:** re-resolve price + RE-RUN the risk floor (equity floor or PM floor by asset_class) against fresh state — never trust the draft's stored verdict.
5. **Daily-loss gate:** if today's realized loss (running tally) ≥ `cli.risk.max_daily_loss`, OR this order's max-loss would push it over, → reject "daily loss limit reached". (Realized-only, UTC-day boundary — see Storage.)
6. **Reserve** (atomic `reserve_for_submit`) → execute:
   - equity → `UnifiedTrading::place_order(account_id, order)` (account-aware → broker by the account's mode).
   - PM → `PredictionExchangeRegistry::adapter(venue)->place_order(OrderRequest)` (the live adapter path; first real use, fully gated).
7. **Record:** update draft, append `trade_audit` (mode "live", account, broker order id + response), update the daily-realized-P&L tally on a fill.
`mode:"paper"` is unchanged (Phase A/B). The kill switch (step 1) is added to the paper path too.

### The kill switch (`cli.kill_switch`)
A single GUI-only boolean, checked FIRST on every `submit_order` (paper and live) and `prepare_order` (so the AI can't even draft while halted — optional; at minimum submit). True → immediate refusal, no execution, audited "kill_switch". The panic button: a human flips it and the AI stops trading instantly (next-submit, no replay). Always available, always honored.

### Daily-loss enforcement (`cli.risk.max_daily_loss`)
The Phase A/B cap was recorded but not enforced. Phase C enforces it as a **running realized-P&L tally**:
- A new table (or `trade_audit`-derived view) tracks realized P&L per UTC day, updated on each fill (equity: from the broker fill / position close; PM: from sell-to-close proceeds vs cost basis; live: from the broker/adapter fill).
- At submit, sum today's realized loss; if `today_realized_loss >= max_daily_loss` → halt (reject all new orders, like a soft kill until the UTC day rolls over or the human resets). Deterministic, in the daemon, outside AI control.
- **Scope decision:** realized-only (simplest, deterministic, no live-mark dependency). Unrealized drawdown limits are a follow-up.

### Daemon / headless
- The `submit_order` carve-out is ALREADY correct for live (returns `cli_trading_allowed() && cli_live_armed()`); no checker change. Phase C only changes the handler (execute instead of hard-off) + adds the kill-switch/account/daily-loss gates (all in the handler, deterministic).
- Live credentials live in `AccountManager`/`SecureStorage` (existing); the daemon loads them for the allowed account. The AI never sees credentials.
- Off-thread dispatch (shipped) keeps a slow broker call from wedging the daemon.

### Storage
- Reuse `order_drafts` + `trade_audit` (add live fields to the audit snapshot: account, broker_order_id, broker mode). 
- **New (migration vNNN, next after v050):** a `daily_pnl(utc_day TEXT PK, realized_pnl REAL, updated_at)` table (or compute from `trade_audit` fills) for the daily-loss tally.

## Data flow (one live cycle, sandbox or real — identical code)
Human arms (GUI) + designates a sandbox account → AI `prepare_order` (validate + floor + draft) → `submit_order(draft_id, "live")` → daemon: kill-switch? armed? account allowed? floor (fresh)? daily-loss ok? → reserve → `UnifiedTrading::place_order(allowed_account, order)` / PM `adapter->place_order` → broker sandbox fill → audit (mode live) + update daily P&L → return the fill. Swapping the allowed account to live credentials (human) makes the identical flow real money.

## Error handling
Structured rejections, distinct reasons: kill-switch ("kill switch engaged"), not-armed ("live trading not armed — arm in GUI Settings"), account ("account not allowed for AI trading"), risk (the floor's reasons), daily-loss ("daily loss limit reached"), broker error (the broker/adapter's message, audited). A broker rejection is a clean tool result, not a crash.

## Testing strategy (Phase C) — NO real money
- **Deterministic gates (no network, the safety core):** kill-switch halts paper+live; live denied when not armed (either toggle off); account-not-allowed denied; daily-loss halt at the cap; the floor re-runs fresh at submit. All unit-tested with a FAKE broker + FAKE PM adapter that simulate fills, and via the daemon e2e (gates need no network).
- **Sandbox live path (real API, fake money):** with an Alpaca sandbox/paper account configured + armed, `submit_order live` actually fills on the broker sandbox; assert the fill + the live audit row + the daily-P&L update. (Skipped/best-effort when no sandbox credentials are present in the test env — documented; the deterministic gates are the hard asserts.)
- **AI-can't-arm (keystone, extended):** `set_setting` refuses `cli.allow_trading`/`cli.live_trading_armed`/`cli.allowed_account`/`cli.kill_switch`/`cli.risk.max_daily_loss` even with settings-write on.
- **Revocable:** un-arm / engage kill switch / clear allowed-account mid-session → the next submit is refused.
- **No-regression:** Phase A equity paper + Phase B PM paper unchanged; the existing carve-out still denies non-submit_order destructive over the daemon.

## Risks (this is the real-money phase — highest stakes)
- **Real-money execution exists in code.** Mitigation: it only fires when a human has configured real credentials AND set the account mode `live` AND allowed it AND armed both toggles AND not engaged the kill switch — a 5-condition human gate the AI cannot touch. Development + automated tests run only against sandbox accounts.
- **First real use of the adapter's live `place_order`** (PM) and the account-aware broker path. Mitigation: fully gated, fresh-rechecked, reserved, audited; sandbox-validated; submit-only (no cancel/replace surface yet).
- **Daily-loss tally correctness** (realized P&L across fills/venues). Mitigation: realized-only + UTC-day; deterministic; conservative (halts on breach); unit-tested.
- **Credential exposure.** The AI never reads credentials (AccountManager/SecureStorage); only the daemon uses them for the allowed account.
- **PM sandbox availability** is uneven (Kalshi has a demo env; Polymarket is on-chain — testnet support via the adapter is uncertain). Mitigation: equity (Alpaca sandbox) is the primary validated live path; PM live validation may be limited to the deterministic gates + manual/real-cred testing by the human. Flagged as an open item.

## Follow-ups (out of Phase C scope)
- The live AI action set beyond submit: cancel_order/replace_order/exit_position (each via the same checker-opens-door → handler-is-authority pattern, armed-only).
- Unrealized-drawdown limits; intraday kill-switch auto-trip on breach; per-venue daily-loss.
- PM live testnet/sandbox coverage (Polymarket on-chain test env).
- The `openterminalcli ai run strategy <name> --mode live` continuous loop.
- Honor PM `limit_price` as a live execution constraint (Phase B deferred).

## Open questions (resolve before/in the plan)
- Daily-loss reset: UTC midnight (proposed) vs broker trading session.
- Does `prepare_order` also honor the kill switch (proposed: yes — refuse to even draft while halted), or submit-only?
- PM live validation target given uncertain Polymarket testnet (proposed: equity Alpaca sandbox is the validated live path; PM live gated + unit-tested with a fake adapter + deterministic-gate e2e, real PM live left to the human with real creds).
