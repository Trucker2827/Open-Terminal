# Fast Live Mode — Design Spec (Phase D)

**Status:** Approved policy (user-locked) — precedes the plan. **Real-money phase — sandbox-validated build.**
**Date:** 2026-06-15
**Builds on:** Phase A–C (the two-phase substrate, the deterministic risk floor, the daily-loss tally, the GUI-only `cli.` constitution incl. the kill switch + `cli.allowed_account`, the daemon/headless/GUI auth-checkers, the `submit_order` carve-out on all three hosts) and the live rails (`UnifiedTrading` account-aware `place_order`/`cancel_order`/`close_position`/positions/orders via `AccountManager`/brokers).

## Goal

A higher human-arming tier — **fast live mode** — that unlocks a fuller, **low-latency** live action set for the AI, because *for speed the AI needs direct live broker actions* (manage orders/positions in one call, not a draft-then-confirm round trip). The human owns the arena; once they arm fast mode, the AI plays fast inside it. The constitution is unchanged: the AI can act, but can never move the walls.

## Policy (user-locked)

**ALLOWED when the human arms fast live mode** (new gated tools — NOT raw adapter):
- `fast_submit_order` — a **one-shot** live order (validate + gate + risk floor + execute in a single call; no prepare/draft/re-check).
- `cancel_order` — cancel an open live order.
- `replace_order` — modify an open live order.
- `exit_position` — close/reduce a live position (reduce-only).
- `get_positions`, `get_open_orders`, `get_fills` — read live broker state for the allowed account.

**STILL DENIED** (constitutional, enforced unchanged):
- Raw adapter calls / the direct `live_*` broker tools (the AI never gets the raw rail).
- Changing any `cli.*` setting (the `cli.` prefix keystone — `set_setting` refuses).
- Changing the risk caps (`cli.risk.*`, GUI-only).
- Changing the allowed account (`cli.allowed_account`, GUI-only).
- Disabling the kill switch (`cli.kill_switch`, GUI-only; checked first; revocable).

## The arming tiers (two-layer constitution, extended)

| Tier | Human sets (GUI-only) | AI may then do |
|---|---|---|
| Paper | `cli.allow_paper_trading` | prepare/submit paper |
| Live (careful) | + `cli.allow_trading` + `cli.live_trading_armed` + `cli.allowed_account` | `submit_order mode:live` (two-phase) |
| **Fast live (NEW)** | + **`cli.fast_live_armed`** | the **fast action set** above |

Fast live mode requires the FULL base live arm AND the new `cli.fast_live_armed` AND an allowed account AND the kill switch off — a deliberate, higher human opt-in for the more powerful (cancel/replace/exit/one-shot) set. All five are `cli.*` → GUI-only → the AI cannot self-arm. Any of them off (or kill switch on) → the fast tools are denied / refuse.

## Architecture

### Gated wrappers, not raw rails
Each new tool is a **gated MCP wrapper** (the same pattern as `submit_order`) over the existing `UnifiedTrading` account-aware broker ops — it is NOT the raw `live_*` tool. The raw `live_*` tools stay denied on the AI hosts (daemon denies all destructive; headless `is_live_execution_tool` denies; GUI confirmation-gates). The new tools route ONLY to `cli.allowed_account` (never an AI-supplied account), and each runs the gate stack before touching the broker.

### Per-tool gate stack (all gates read LIVE — revocable)
1. **Kill switch** (`cli.kill_switch`) — checked first; engaged → refuse everything.
2. **Fast-armed** — `cli_trading_allowed() && cli_live_armed() && cli_fast_live_armed()` → else "fast live mode not armed".
3. **Allowed account** — resolve `cli.allowed_account`; must exist + be the route; any AI-named account must equal it → else "account not allowed".
4. **Risk floor** — `fast_submit_order` + `replace_order` run the deterministic risk floor (order value / position size / daily-loss) on the (new) order params. `cancel_order` / `exit_position` are **de-risking** (reduce/close) → skip the order-value floor (they can only reduce exposure), but still require armed + allowed-account + kill-off. Reads (`get_*`) require armed + allowed-account, no floor.
5. **Execute** via the account-aware `UnifiedTrading` op; record to the live P&L ledger where applicable (a fill from `fast_submit_order`/`exit_position` updates the daily-loss tally); **audit** every action (`trade_audit`, mode "live", the tool, the broker order id + response).

`fast_submit_order` is **one-shot**: it performs steps 1–5 in a single call (no `order_drafts` row, no separate re-check) — the latency win — while still passing the full gate stack atomically.

### Hosts
- The new tools are added to the carve-out in all three auth-checkers (`ServeCommand`, `HeadlessRuntime`, `AgentService`) via a shared classifier (e.g. `is_fast_live_tool(name)`): allowed only when `cli_fast_live_armed()` (+ the base arm); denied otherwise. Reads (`get_*`) are non-destructive but account-scoped → also gated on fast-armed (they expose live broker/account state). The raw `live_*` denial is unchanged.
- Off-thread dispatch (shipped) keeps a slow broker call from wedging the daemon.

### Storage
- Reuse `trade_audit` (every fast action) + the Phase-C `live_positions`/`daily_pnl` ledger (fills update the realized tally). A new migration only if `replace_order`/order tracking needs state (proposed: none — the broker is the source of truth for open orders; the substrate audits + tallies fills).

## Validation (NO real money)
- **Deterministic gates (the safety core), fake broker:** fast-not-armed → denied; kill-switch → refused; account-not-allowed → refused; risk floor rejects an oversized `fast_submit`/`replace`; daily-loss halt; cancel/exit allowed when armed (de-risking) and refused when not armed; the raw `live_*` tools still denied; the AI can't set `cli.fast_live_armed`/caps/account/kill-switch (keystone). Unit-tested with a FakeBroker + a fake adapter.
- **Sandbox live path (real API, fake money):** with an Alpaca sandbox account + fast mode armed, `fast_submit_order` → real sandbox fill; `cancel_order`/`replace_order`/`exit_position` operate on sandbox orders/positions; `get_positions`/`get_open_orders`/`get_fills` return sandbox state. Skipped/best-effort when no sandbox creds in the test env.
- **No real money in dev/tests.** Real money = the human configures a `live`-mode account + arms fast mode.

## Error handling
Structured rejections, distinct reasons: kill-switch ("kill switch engaged"), not-armed ("fast live mode not armed — arm in GUI Settings"), account ("account not allowed for AI trading"), risk ("exceeds max order value", "daily loss limit reached"), broker error (the broker's message, audited). De-risking actions (cancel/exit) on a missing order/position → a clean "not found", not a crash.

## Risks (real-money phase)
- **Fuller live surface (cancel/replace/exit/one-shot).** Mitigation: each is a gated wrapper behind the 5-condition human arm + the deterministic floor + the kill switch; raw adapter stays denied; sandbox-validated; submit/replace risk-floored; cancel/exit are de-risking only.
- **One-shot skips the two-phase re-check.** Mitigation: it still runs the full gate stack atomically (kill/armed/account/floor/daily-loss) — the two-phase re-check guarded against stale *drafts*, which one-shot doesn't create (it validates against fresh state in the same call).
- **Daily-loss reconciliation on live fills** — the known Phase-C follow-up (broker-fill reconciliation); fast fills feed the same realized tally with the same documented limitation.
- **Credential exposure** — the AI never sees credentials; only the daemon/host uses them for the allowed account.

## Follow-ups (out of scope)
- Broker-fill reconciliation (poll fills → exact realized P&L) — shared with Phase C.
- `replace_order` partial-fill semantics; bracket/OCO orders; multi-account.
- A fast-mode strategy-loop variant (the `ai run strategy` loop using `fast_submit_order`).

## Open questions (resolve in the plan)
- Whether `exit_position` flattens fully or supports partial (proposed: full close in v1; partial = a qty param later).
- Whether `replace_order` re-runs the full floor on the delta or the whole new order (proposed: the whole new order — simplest + safe).
