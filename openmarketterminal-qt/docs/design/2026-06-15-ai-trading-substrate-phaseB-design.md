# AI-Trading Substrate — Phase B (Prediction-Market Paper) Design Spec

**Status:** Approved design (pending user spec review) — precedes the plan.
**Date:** 2026-06-15
**Builds on:** the shipped Phase A substrate (`prepare_order`/`submit_order`, deterministic risk floor, `order_drafts`+`trade_audit`, the GUI-only `cli.` constitution, the daemon/headless `submit_order` carve-out) and the existing prediction infrastructure (`PredictionExchangeAdapter`, `PredictionExchangeRegistry`, `PredictionTypes`, Polymarket service+WebSocket, Kalshi adapter).
**Parent spec:** `2026-06-14-ai-trading-substrate-design.md` (the two-layer constitutional model + phasing).

## Goal

Extend the *same* safety substrate to **prediction markets, paper-only**: let the AI browse markets, propose contract trades, and paper-execute them on **Polymarket AND Kalshi** through the identical two-phase `prepare_order` → `submit_order` flow, with a deterministic PM-specific risk floor it cannot bypass, full audit, and **live hard-off** (live PM is Phase C). The point is unchanged: measure whether the AI has real edge on prediction markets, with the daemon as the final authority and the human owning the constitution.

**User decisions (this spec is built on these):**
- **Venue:** both Polymarket and Kalshi in Phase B (behind the substrate; each is a registered `PredictionExchangeAdapter`).
- **Fill model:** *best available price, immediate* — a paper BUY fills at the best ask, a SELL at the best bid (from the live order book / last price); the whole order fills at once. (Proven-analogous to the equity paper rail; book-depth/slippage is a follow-up.)
- **Settlement:** *mark-to-market only* for the MVP — positions stay open and are marked against the live probability; P&L is unrealized. Settle-at-resolution (YES→$1 / NO→$0) is a Phase B+ follow-up (needs a resolution feed).

## What already exists vs what Phase B builds

**Exists (GUI-tier, reuse):**
- `PredictionExchangeAdapter` — async (signal/slot) interface: `list_markets/search/fetch_market/fetch_order_book/fetch_price_history`, `place_order(OrderRequest)`, `cancel_order`, `subscribe_market`, `fetch_balance/positions`. Live (real creds via `PredictionCredentialStore`).
- `PredictionExchangeRegistry::instance().adapter(id)` / `available_ids()` — one adapter per exchange id, registered in `main.cpp`.
- `PredictionTypes`: `MarketKey{exchange_id,market_id,event_id,asset_ids}`, `PredictionMarket{question,category,end_date_iso,volume,liquidity,open_interest,outcomes,tags}`, `Outcome{name "Yes"/"No",asset_id,price 0–1}`, `PredictionOrderBook{bids/asks: OrderLevel{price,size}, tick_size, min_order_size}`, `OrderRequest{key,asset_id,side "BUY"/"SELL",order_type,price 0–1,size contracts,...}`.
- `ThreadHelper::run_async_wait(target, start)` — the async→sync bridge MCP read tools use (call adapter method, wait for the `*_ready` signal with a timeout; runs fine off the daemon worker thread, exactly like `get_quote`).

**Phase B builds:**
1. **PM read tools** (non-destructive MCP) so the AI can see markets/prices and form intents.
2. **A PM paper engine** + storage — there is NO paper rail for PM today (`place_order` is live). This is the analog of the equity paper portfolio.
3. **The two-phase flow extended to a PM intent** (one unified `prepare_order`/`submit_order`, discriminated by `asset_class`).
4. **A deterministic PM risk floor** (exposure-per-topic, liquidity/spread, settlement-proximity, max-loss-from-contracts) on GUI-only caps.
5. **Headless/daemon registration of the PM adapters** (today only the GUI `main.cpp` registers them).

## Architecture

### Two-layer constitution (unchanged; new walls are free)
Per the parent spec, the human owns the arena; the AI plays inside it. Phase B adds two **constitutional controls**, GUI-only automatically because the keystone blocks the whole `cli.` prefix:
- `cli.allowed_venues` — comma-list of venues the AI may trade (e.g. `polymarket,kalshi`); empty = none. The AI can read it, never write it.
- `cli.risk.max_exposure_per_topic` — max summed stake ($) across one market category/topic.
The existing `cli.allow_paper_trading` gates PM paper submit too (same toggle, same revocable live-read). The AI cannot enable a venue, raise the exposure cap, or arm live — all are `cli.*` writes `set_setting` refuses.

### The PM intent (what the AI sends)
A strict JSON object with `asset_class:"prediction"`:
```json
{ "asset_class":"prediction", "venue":"polymarket",
  "market_id":"0x...", "asset_id":"0x...", "outcome":"YES",
  "side":"buy", "contracts":100, "limit_price":0.62,
  "strategy":"news_edge", "reason":"resolution likely YES; price lags the signal" }
```
`limit_price` is a probability in [0,1]; `contracts` is the number of shares; `outcome` identifies the YES/NO (or multi-outcome) leg via its `asset_id`. `strategy`/`reason` are audited, never trusted for control. (Equity intents keep their existing shape; `asset_class` defaults to `"equity"` when absent, so Phase A callers are unchanged.)

### Two-phase flow — unified, discriminated by `asset_class`
**One** `prepare_order` / `submit_order` pair (NOT parallel `pm_*` tools) — the substrate, draft pipeline, audit, gate ladder, and the daemon carve-out are shared; only validation, the risk floor, and execution branch on `asset_class`. (Alternative considered: separate `pm_prepare_order`/`pm_submit_order` — rejected: it duplicates the carve-out + audit and fragments the constitution. The union schema is the smaller surface.)
- **`prepare_order`** (non-destructive): if `asset_class=="prediction"` → validate (venue in `cli.allowed_venues`; market/asset resolves via the adapter; outcome valid; `contracts>0`; `limit_price∈[0,1]` for limit), resolve the current price (best ask for BUY / best bid for SELL via `fetch_order_book`), run the **PM risk floor**, draft to `order_drafts`, audit `prepare`. Returns `{status:"prepared", draft_id, risk_status, max_loss, est_cost, checks:[...]}`.
- **`submit_order`** (destructive, the carve-out): load draft → re-resolve price + **re-run the PM floor fresh** → gate by mode. `mode:"paper"` requires `cli_paper_trading_allowed()` (revocable) → execute on the **PM paper engine** (record the position at the best-available fill price), atomic `reserve_for_submit` first, audit `submit`. `mode:"live"` → **hard-off** ("live trading disabled (paper-first; not yet enabled)"), zero adapter calls. Live PM is Phase C.

### PM paper engine (new)
A lightweight, deterministic engine + storage — the PM analog of `pt_*` equity paper:
- **`pm_paper_positions`** table: `id, venue, market_id, asset_id, outcome, side, contracts, avg_price, cost_basis, opened_at, status`. A paper BUY opens/adds; a paper SELL of an owned outcome reduces/closes (or opens a short, see below).
- **`pm_paper_account`** cash balance (seed e.g. $100k, mirroring the equity paper portfolio) — a BUY debits `contracts×fill_price`, a close credits proceeds. (A simple single-account model for the MVP; multi-portfolio is a follow-up.)
- **Fill:** best available price, immediate, whole order (the decided model). Fill price = best ask (BUY) / best bid (SELL) from the live `fetch_order_book`; if no book/price is resolvable → reject ("no price available for risk check"), never fill blind.
- **Mark-to-market:** unrealized P&L = `(current_price − avg_price) × contracts` for a long YES; computed live from the adapter price. No settlement/resolution payout in Phase B (deferred).
- **Reads:** a `pm_paper_portfolio` MCP read tool returns cash, positions, and live mark-to-market P&L (so the AI can see its own paper book).

### PM read tools (non-destructive MCP)
Minimal set so the AI can form intents (all `auth None`, `is_destructive=false`, async→sync via `run_async_wait` against the registry adapter):
- `pm_list_markets(venue, category?, sort_by?, limit?)`, `pm_search_markets(venue, query, limit?)`, `pm_get_market(venue, market_id)` (question, outcomes, prices, end_date, liquidity), `pm_get_order_book(venue, asset_id)`, `pm_get_price(venue, asset_id)`.
- `pm_paper_portfolio()` (cash + positions + mark-to-market).
These reach their handlers over the daemon (non-destructive → the read-only gate already allows them).

### Deterministic PM risk floor (GUI-only caps, AI cannot bypass)
At BOTH prepare and submit (re-checked fresh), in dollars where stake = `contracts × fill_price`:
- **Order value** (`cli.risk.max_order_value`, reused): reject if `contracts × fill_price > cap`.
- **Position size** (`cli.risk.max_position_qty`, reused): reject if `contracts > cap`.
- **Exposure per topic** (`cli.risk.max_exposure_per_topic`, NEW, finite default e.g. $10k): reject if this trade would push summed open stake in the market's `category` over the cap.
- **Liquidity / spread guard** (NEW, finite defaults): reject if the market `liquidity < cli.risk.pm_min_liquidity` or the book spread `> cli.risk.pm_max_spread` (thin/illiquid PM books are where paper results lie).
- **Settlement-proximity guard** (NEW): reject if `end_date_iso` is within `cli.risk.pm_min_hours_to_resolution` (default e.g. 1h) — no trading into resolution.
- **Max-loss-from-contracts** (recorded + bounded): a BUY can lose its full stake (`fill_price × contracts`); a SELL/short can lose `(1 − fill_price) × contracts`. Recorded in the verdict + audit. (The running daily-loss tally remains Phase C, as in equity.)
All caps come only from GUI-only `cli.risk.*` keys with finite defaults (empty/≤0 → default, never "unlimited"), exactly like Phase A. The AI's `contracts`/`limit_price`/`venue` are inputs to these checks, never overrides.

### Storage
- **Reuse** `order_drafts` (intent_json holds the PM intent incl. `asset_class`/`venue`) + `trade_audit` (phase/decision/reason/risk_snapshot, with `mode` and the venue in the snapshot). No schema change needed for drafts/audit.
- **New migration vNNN** (next after v049): `pm_paper_positions` + `pm_paper_account` (or a `pm_paper_portfolio` row). Repositories mirror the Phase A pattern (`BaseRepository`, atomic where it matters).

### Daemon / headless integration
- The PM adapters are registered today only in the GUI `main.cpp`. Phase B registers them in the **headless runtime** bring-up (alongside `register_all_data_services`/`register_core_tools`) so the daemon can serve PM read + paper tools. Paper needs only public market data + the paper store — **no live PM credentials**. (Live creds remain a Phase C concern.)
- The `submit_order` carve-out is unchanged (it keys on `tool=="submit_order"` + `mode`); PM paper rides the existing paper carve-out. Every other destructive PM action (the adapter's live `place_order`/`cancel_order`) stays denied over the daemon — they are not exposed as carved-out tools in Phase B.

## Data flow (one PM paper cycle)
AI `pm_search_markets`/`pm_get_market`/`pm_get_order_book` → forms a PM intent → `prepare_order` → daemon validates + PM risk floor + drafts → `submit_order(draft_id,"paper")` → daemon re-checks fresh + gate (`cli.allow_paper_trading` + `cli.allowed_venues`) + reserve + fills at best price on the paper engine + records the position + audits → AI reads `pm_paper_portfolio` for mark-to-market. Live is the same path, **disabled in Phase B**.

## Error handling
Structured rejections (`{status:"rejected", reason, checks?}`), distinct reasons: venue-not-allowed ("venue 'X' not in allowed venues — enable in GUI Settings"), exposure ("exceeds max exposure for topic 'Y'"), liquidity/spread ("market too illiquid / spread too wide"), proximity ("too close to resolution"), gate-off ("paper trading disabled"), live-off ("live trading disabled"), no-price ("no price available for risk check").

## Testing strategy (Phase B)
- **Happy PM paper path** (each venue): search→get market→prepare valid intent within caps→`prepared` + draft; submit paper with `cli.allow_paper_trading` on and venue allowed → fills at best price, a `pm_paper_positions` row exists, `pm_paper_portfolio` shows the position + mark-to-market; audit rows for both phases.
- **PM risk floor:** oversized stake → prepare rejects; over-exposure-per-topic → rejects; too-wide-spread / too-thin-liquidity → rejects; within-N-hours-of-resolution → rejects; a cap lowered between prepare and submit → submit re-check rejects (revocable).
- **Constitution:** `set_setting cli.allowed_venues` / `cli.risk.max_exposure_per_topic` (even with settings-write on) → refused (keystone, prefix rule); a venue not in `cli.allowed_venues` → submit rejected.
- **Gate ladder:** paper off → denied; live → "live disabled" regardless.
- **No-regression:** equity Phase A path (`asset_class` absent/`"equity"`) still works unchanged; existing selftests/e2e pass; the daemon still denies the adapter's live PM tools.
- Use the seam pattern (injected/faked adapter or a fixture order book) so tests don't depend on live Polymarket/Kalshi network — mirror how the equity tests avoid live quotes.

## Risks
- **Two venues at once** doubles the per-exchange quirks (Polymarket asset_id/neg-risk vs Kalshi ticker/cents). Mitigation: both go through the single `PredictionExchangeAdapter` abstraction; the paper engine + risk floor are venue-agnostic (operate on price 0–1 + contracts); per-venue specifics stay in the adapters.
- **Live market data dependency for fills** — paper fills use the *live* book/price; a network/adapter failure → reject (no blind fill), same fail-safe as equity market orders.
- **Mark-to-market ≠ realized edge** — without settlement, P&L is unrealized; settle-at-resolution (the true PM test) is the first Phase B+ follow-up. Documented, not hidden.
- **Headless adapter bring-up** — registering GUI-tier adapters in the headless runtime is the main new integration surface; the plan grounds it against how `register_all_data_services` brings up services headless.

## Follow-ups (out of Phase B scope)
- **Settle-at-resolution**: a resolution feed/poll → realize YES→$1/NO→$0 payouts; the honest measure of PM edge.
- **Book-depth fills / slippage**; multi-portfolio paper accounts; partial fills.
- **Phase C live PM**: the adapter's live `place_order`/`cancel_order` behind `cli.allow_trading`+`cli.live_trading_armed` + the PM floor + the carve-out extended to cancel/replace (per the parent spec's Phase C).
- Coinbase PM adapter (availability uncertain).

## Resolved decisions
- **BUY-to-open + SELL-to-close ONLY** for Phase B (user, 2026-06-15). A paper SELL may only reduce/close an existing long position in that `asset_id`; SELL-to-open (shorting) is rejected ("no open position to sell; short-open is not enabled in Phase B"). This keeps the max-loss model simple (a long can lose at most its stake `fill_price × contracts`) and the position model single-sided.
- Exposure "topic" = market `category` (revisit if too coarse).

## Open questions (non-blocking — tune in the plan)
- Paper account seed size (proposed $100k, mirroring the equity paper portfolio).
- Default finite values for the new PM caps (`pm_min_liquidity`, `pm_max_spread`, `pm_min_hours_to_resolution`, `max_exposure_per_topic`).
