# Portfolio Account Sync — Design Spec

**Date:** 2026-07-07
**Status:** Approved (brainstorming) — pending implementation plan
**Goal:** Stop the Portfolio screen from being a manual ledger. One action mirrors
every connected account — equity brokers (Alpaca, IBKR, Tradier, …) and crypto
exchanges (Coinbase, Kraken, …) — into per-account "synced" portfolios, plus a
live **All Accounts** aggregate, all reported in the portfolio base currency.

**Non-goal / hard constraint:** This feature is **read-only**. It fetches
holdings/positions/cash only (`get_holdings` / `get_positions` / `get_funds` /
ccxt `fetch_balance`). It NEVER places, modifies, or cancels orders, and touches
none of the live-trading gates (`allow_trading` / `live_trading_armed` / etc.).

---

## Decisions (from brainstorming)

1. **Data model:** per-account synced portfolios + a virtual live **All Accounts**
   aggregate view (NOT one merged portfolio). Fits the existing one-account-per-
   portfolio model.
2. **Sync semantics:** **mirror the account exactly** — on each sync the synced
   portfolio's holdings are reconciled to match the account (add / update /
   remove). Synced portfolios are read-only. **Manual portfolios are never
   touched.**
3. **Crypto cost basis:** crypto reports quantity but not cost basis → show
   **market value only**, P&L badged unavailable, excluded from P&L totals but
   still counted toward NAV. Equities keep full P&L from the broker's avg cost.
4. **Trigger:** a "Sync accounts" button (syncs all connected accounts) + auto-
   sync when a synced / All-Accounts view is opened; a "last synced Xm ago"
   label. Prices stay live via the existing 60s refresh timer.
5. **Cash:** included as a per-account pseudo-holding line (broker buying power
   via `get_funds`; crypto fiat via `fetch_balance`), market value = cash, no
   P&L, so NAV reflects real account value.

---

## Architecture

### Components

- **`AccountSyncService`** (new, `src/services/portfolio/AccountSyncService.{h,cpp}`)
  — orchestrates a sync run. Enumerates connected accounts, invokes the matching
  `IAccountSource` for each, and hands the normalized holdings to the mirror
  writer. Emits `sync_started` / `account_synced(source, ok, error)` /
  `sync_finished(summary)` so the UI can show progress and a "last synced" time.

- **`IAccountSource`** (new interface) — `QVector<SyncedHolding> fetch(const
  AccountRef&)` + `QString base_currency(const AccountRef&)`. Two implementations:
  - **`EquityAccountSource`** — enumerates via `AccountManager::list_accounts()`;
    fetches `IBroker::get_holdings` (symbol, qty, avg cost) + `get_funds` (cash)
    through `BrokerRegistry`. Fills `has_cost_basis = true`.
  - **`CryptoAccountSource`** — enumerates crypto exchanges that have stored
    credentials (`ExchangeSessionManager::supported_exchange_ids()` filtered by
    `SecureStorage` key presence `crypto:<exchange>:*`); fetches `fetch_balance()`
    per exchange. Each non-zero coin balance → a `SyncedHolding` with
    `has_cost_basis = false`; fiat balances (USD/USDT/…) → the cash line.

- **`SyncedHolding`** (new struct) — normalized: `{ canonical_symbol, quantity,
  avg_cost, has_cost_basis, native_currency, broker_symbol, exchange }`.

- **Mirror writer** (`AccountSyncService::mirror_into_portfolio`) — reconciles the
  account's synced portfolio's `portfolio_assets` against the fetched list:
  upsert matched (by canonical symbol), add new, **remove vanished**. Runs ONLY
  on a successful fetch; a failed/empty-error fetch leaves the portfolio intact
  and flags it stale (`sync_error` recorded, `synced_at` untouched).

- Reused: `PortfolioService` (add/update/remove assets, summary), the pure
  `build_summary` (FX conversion + snapshot gate), `PortfolioHoldingDisplay`
  (badge helper), `MarketDataService` (quotes + currency + FX).

### Storage (migration)

One migration on the `portfolios` table:
- `sync_source TEXT DEFAULT ''` — `"broker:<account_id>"` or `"crypto:<exchange>"`.
  Non-empty ⇒ the portfolio is **sync-owned**: read-only in the UI, mirror-managed
  by `AccountSyncService`.
- `synced_at TEXT DEFAULT ''` — ISO timestamp of the last successful sync.
- `sync_error TEXT DEFAULT ''` — last error (empty when healthy); drives the
  "stale" badge.

Equity synced portfolios also set the existing `broker_account_id` so live quotes
route through the broker (as today). Crypto synced portfolios leave
`broker_account_id` empty (quotes route via yfinance using the `BTC-USD` canonical
symbol).

A per-asset column is NOT needed: each synced portfolio holds exactly one
account's holdings, so the source is the portfolio's `sync_source`.

### Symbol normalization

- Crypto: ccxt `BTC/USD` → canonical `BTC-USD` (yfinance format) so the existing
  quote + FX pipeline prices it; the ccxt pair is retained in `broker_symbol` and
  the exchange id in `exchange`.
- Equity: symbols pass through unchanged; `broker_symbol` + `exchange` stored as
  the existing import does.

### All Accounts aggregate

A virtual selector entry (`id = "__all_accounts__"`), not a DB row.
`PortfolioService::load_summary` special-cases it:
1. Load every sync-owned portfolio (`sync_source != ''`).
2. Union their assets and **merge duplicate canonical symbols** across accounts
   (e.g. AAPL at Alpaca + IBKR): summed quantity, quantity-weighted average cost;
   `has_cost_basis` for the merged row is true only if every contributor has cost
   basis.
3. Run the same `build_summary` (with FX conversion to the single base currency)
   → one summary. Snapshots for the aggregate follow the same `snapshot_safe()`
   gate.

The merge is a pure function (`aggregate_holdings(QVector<portfolio_assets by
portfolio>) -> QVector<PortfolioAsset>`), unit-tested independently.

---

## `build_summary` / display changes

- **`has_cost_basis` per holding.** Add to `PortfolioAsset` (stored) and thread
  through `HoldingWithQuote`. When false in `build_summary`:
  - cost_basis / unrealized_pnl / unrealized_pnl_percent are NOT meaningful → set
    to 0 and mark the holding, and
  - **exclude the holding from `total_cost_basis` / `total_unrealized_pnl`** (its
    market value still contributes to `total_market_value` / NAV).
- **Display badge.** `PortfolioHoldingDisplay::price_dependent_cells` gains a
  third muted case: a holding with `has_cost_basis == false` shows its price /
  market value normally but dashes P&L / P&L% with a "no cost basis from exchange"
  tooltip. (LAST / MKT VAL stay real for crypto — only the P&L columns dash.)

  This refines the current all-or-nothing muting into per-column: unpriced / FX-
  unresolved dash the whole price block; missing-cost-basis dashes only the P&L
  columns.

- **Cash line.** A cash pseudo-holding is a `PortfolioAsset` with a reserved
  symbol convention (`symbol = "$CASH:<CCY>"`, e.g. `$CASH:USD`, quantity = cash
  amount, avg_cost = 1, `has_cost_basis = false`). `build_summary` recognizes the
  `$CASH:` prefix and treats it specially: **priced = true at price 1.0** in the
  cash's native currency (so it is NEVER sent to the quote fetch and never trips
  the unpriced/snapshot gate), FX-converted like any holding, P&L dashed. So
  market value = cash amount in base currency, contributing to NAV with no P&L.
  Rendered as a "Cash" row. (This is why cash carries a currency in the symbol —
  a Kraken EUR balance and an Alpaca USD balance convert correctly.) The FX
  orchestration in `PortfolioService::build_summary` parses the `$CASH:<CCY>`
  suffix to get the cash's native currency (rather than the `currency_code`
  symbol lookup, which only knows real tickers) and resolves its `<CCY><base>=X`
  rate the same way.

---

## UI

- **Portfolio screen toolbar:** a `⟳ Sync accounts` button + a "last synced Xm
  ago" label (per-portfolio for a synced portfolio; most-recent across accounts
  for All Accounts).
- **Portfolio selector:** synced portfolios appear with their account name
  (Alpaca / Coinbase / Kraken), plus the virtual **All Accounts** entry at top.
  Synced portfolios show a small "synced" indicator; a stale one (last sync
  failed) shows a warning with the `sync_error` tooltip.
- **Read-only guard:** Add/Sell/Edit actions are disabled for sync-owned
  portfolios (they are account mirrors).
- **First-run:** if no synced portfolios exist yet, the button reads "Sync
  accounts" and, on click, creates one synced portfolio per connected account.

---

## Sync flow

1. User clicks "Sync accounts" (or opens a synced / All-Accounts view).
2. `AccountSyncService::sync_all()` enumerates connected accounts (equity +
   crypto with creds).
3. For each account (independently, failures isolated):
   a. `IAccountSource::fetch()` → normalized `QVector<SyncedHolding>`.
   b. Ensure a synced portfolio exists for that `sync_source` (create on first
      run, named after the account).
   c. On success: mirror-reconcile the portfolio's assets to the fetched list;
      set `synced_at`, clear `sync_error`.
   d. On failure: leave assets intact; set `sync_error`; leave `synced_at`.
4. `PortfolioService::refresh_summary` re-runs for the affected portfolio(s) /
   All Accounts, pricing + FX-converting via the existing pipeline.

---

## Error handling

- **Per-account isolation:** one account's failure (disconnected, missing creds,
  rate-limited, network) does not block the others. Its synced portfolio keeps
  its last-good holdings and is flagged stale.
- **Never wipe on failure:** the mirror writer removes vanished positions ONLY
  when the fetch succeeded and returned a (possibly empty) authoritative list. A
  fetch error is not "the account is empty."
- **Empty vs error:** a source distinguishes "successfully fetched, zero
  holdings" (mirror to empty) from "fetch failed" (leave intact) via an explicit
  ok/error result, not an empty vector.
- **Snapshot integrity:** synced summaries flow through the existing
  `snapshot_safe()` gate — unpriced / unresolved-FX holdings still block the NAV
  snapshot. Crypto without cost basis does NOT block (market value is real).

---

## Testing

Pure, DB/network-free units (matching the established pattern):
- **Mirror reconcile** — given current assets + fetched holdings, produce the
  add/update/remove plan. Cases: new position, quantity change, avg-cost change,
  vanished position removed, unchanged no-op, and (critically) **fetch-failed →
  empty plan / no removal**.
- **Aggregate merge** — union across portfolios, duplicate-symbol summing with
  quantity-weighted avg cost, `has_cost_basis` AND-across-contributors, cash
  lines summed.
- **Symbol normalization** — ccxt `BTC/USD` → `BTC-USD`, retains ccxt pair +
  exchange; equity pass-through.
- **`build_summary` cost-basis handling** — `has_cost_basis == false` excludes the
  holding from P&L totals but includes its market value in NAV; badge muting is
  P&L-only for that case.
- **Display badge** — missing-cost-basis dashes P&L columns only, with the right
  tooltip; unpriced / FX cases unchanged.

Fetch + DataHub sides sit behind `IAccountSource` and are stubbed with a fake
source in tests. Every fix ships neuter-verified per project discipline.

---

## Phasing (implementation sequencing)

The equity and crypto sources share the mirror writer, aggregate view, storage,
and UI. Suggested build order (single spec, sequenced tasks):
1. Storage migration + `SyncedHolding` + mirror-reconcile (pure) + tests.
2. `has_cost_basis` through `build_summary` / display + tests.
3. `EquityAccountSource` + `AccountSyncService` (equity path end-to-end).
4. All Accounts aggregate (pure merge + `load_summary` special case) + tests.
5. `CryptoAccountSource` (crypto path) + tests.
6. UI: sync button, selector entries, read-only guard, stale/synced badges.

---

## Out of scope (explicit)

- Placing/canceling orders from the Portfolio screen (read-only feature).
- Reconstructing crypto cost basis from trade history (decided against; may
  revisit).
- One merged portfolio with per-asset source tags (rejected in favor of per-
  account + aggregate).
- Continuous background live-holdings subscription (button + on-open is enough;
  prices already live).
