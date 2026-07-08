# Crypto DOM Price Ladder (SuperLadder) — Design Spec

**Date:** 2026-07-08
**Status:** Approved (brainstorming) — pending implementation plan
**Scope:** Phase 1 — a **read-only** Kraken-Pro-style fixed-price-tick depth-of-market
(DOM) ladder panel for the crypto trading screen. Displays market depth + a
volume-at-price (VAP) heatmap + the user's own resting orders and average entry.
**Click-to-trade is explicitly OUT of scope for this spec** (a separate later phase).

---

## Goal

Give the crypto trading screen a professional DOM **ladder** — the one marquee
piece it lacks versus Kraken Pro. Every row is a fixed price bucket; columns are
`ORDERS | BID | PRICE | ASK | VAP`; bids render below mid, asks above; the user's
working orders and average entry are overlaid. It is a new *view* over data
streams the app already flows (WS order book, time-&-sales trades, live
orders/position) — no new market-data plumbing.

## Non-goals (this spec)

- **No order placement / cancellation.** No click-to-trade, no `place_order` /
  `cancel_order` calls, no wiring to `UnifiedTrading`. Phase 1 ships **zero**
  order-mutation code. The click seam is left unwired.
- Not a replacement for the existing `CryptoOrderBook` — the ladder is an
  additional, separate panel.
- No new WS/daemon data feeds; reuse what `CryptoOrderBook` / `CryptoTimeSales`
  already consume.

## Decisions (from brainstorming)

1. **Row spacing:** a user-selectable **grouping** dropdown (e.g. 0.1 / 1 / 5 /
   10 / 25 / 100). Rows are fixed at that increment; raw depth levels are bucketed
   into each price bucket. No dependency on exchange tick metadata.
2. **Phase 1 content:** market depth (bid/ask) + VAP heatmap **and** the user's
   own resting orders (ORDERS column) + a marker at average entry price — all
   read-only display.
3. **VAP window:** **session** — accumulate all trades since the symbol/exchange
   was opened; reset on symbol or exchange change.
4. **Recenter:** auto-center on mid-price; scrolling detaches and shows a
   "recenter" affordance that snaps back (standard DOM behavior).
5. **Placement:** a new `CryptoLadder` panel alongside the existing order book,
   dockable into a saved workspace via the existing `LayoutCatalog`.

---

## Architecture

### Components

- **`CryptoLadderModel`** (new, pure, header-only:
  `src/screens/crypto_trading/CryptoLadderModel.h`) — all the ladder math, no Qt
  widgets, unit-tested. Holds the session VAP accumulator; computes bucketed
  rows. See "Model" below.

- **`CryptoLadder`** (new widget:
  `src/screens/crypto_trading/CryptoLadder.{h,cpp}`) — a custom-painted
  `QWidget` (mirrors `CryptoOrderBook`'s paint-based approach). Owns a
  `CryptoLadderModel`, subscribes to the live feeds, and paints the rows +
  columns + VAP heatmap + order/position overlays. Hosts the grouping dropdown
  and the recenter affordance.

- Reused (no changes required beyond wiring the ladder to them):
  - The WS **`OrderBookData`** feed (`{symbol, bids[], asks[], best_bid,
    best_ask}`) — same source `CryptoOrderBook` uses.
  - The **`TradeData`** time-&-sales feed (`{side, price, amount, timestamp}`) —
    same source `CryptoTimeSales` uses — for the session VAP.
  - The live **orders** + **position/holdings** feed (the user's resting orders
    and average entry) — the same account stream the crypto screen already
    subscribes to.
  - **`LayoutCatalog`** — to add the ladder as a dockable panel in a saved
    workspace.

### Model (the pure, testable core)

`CryptoLadderModel` exposes pure operations (Qt containers only, no widgets):

```cpp
struct LadderRow {
    double price = 0;       // bucket price (multiple of grouping)
    double bid_size = 0;    // summed bid depth in this bucket (0 if above mid)
    double ask_size = 0;    // summed ask depth in this bucket (0 if below mid)
    double vap = 0;         // session traded volume at this bucket
    double my_bid_qty = 0;  // user's resting BUY qty at this bucket
    double my_ask_qty = 0;  // user's resting SELL qty at this bucket
    bool   is_avg_entry = false; // marker: user's avg entry price falls here
};

struct LadderView {
    QVector<LadderRow> rows; // top (highest price) → bottom (lowest), centered
    double mid = 0;
    double max_depth = 0;    // for depth-bar scaling
    double max_vap = 0;      // for VAP heatmap scaling
};
```

Pure functions:
- `double bucket_price(double price, double grouping)` → `floor(price/grouping)*grouping`.
- `void accumulate_vap(double price, double amount)` → adds to the session VAP
  accumulator, keyed at a **fine base resolution** (the smallest offered grouping
  increment), NOT the current display grouping. `void reset_vap()` on
  symbol/exchange change. Keying at the fine base (and requiring every display
  grouping to be an integer multiple of it) means changing the grouping
  **re-aggregates** VAP correctly instead of corrupting it, and bounds the map
  size. `build()` sums the fine buckets that fall inside each display bucket.
- `LadderView build(const OrderBookData& book, double grouping, int rows_each_side,
  const QVector<MyOrder>& my_orders, double avg_entry_price)` — buckets the book
  around mid, fills VAP from the accumulator, overlays the user's orders and the
  avg-entry marker, and returns the centered window of rows.
  - `MyOrder = {double price; double qty; bool is_buy;}` (a small local struct so
    the model stays free of trading types).

The widget calls `accumulate_vap` on each incoming trade and `build(...)` on each
repaint tick; it never computes bucketing itself.

### Data flow

```
WS OrderBookData  ─┐
                   ├─▶ CryptoLadder (widget) ──▶ CryptoLadderModel.build() ──▶ paint
TradeData (tape) ──┤        │  accumulate_vap() per trade
orders/position ───┘        │  grouping dropdown / recenter state
```

- On symbol or exchange change: `reset_vap()` and clear cached book.
- Repaint is timer-throttled (as `CryptoOrderBook` already does) — the model is
  cheap to rebuild each tick.

### Controls & behavior

- **Grouping dropdown**: options scale to price magnitude; default picks a
  sensible increment from the current mid (e.g. ~4–5 visible significant rows).
- **Recenter**: the view auto-centers on mid each rebuild while "attached". A
  user scroll sets "detached" and shows a recenter affordance; clicking it (or a
  large mid move) re-attaches and re-centers.
- **Columns**: `ORDERS | BID | PRICE | ASK | VAP`. Bids (green) only populate
  rows below mid; asks (red) only above. VAP is a right-aligned horizontal bar
  scaled to `max_vap`. The user's resting orders render in the ORDERS column at
  their bucket; the avg-entry row is marked.

### Error / edge handling

- Empty/na book → render an empty ladder frame with the grouping control (no
  crash, no rows).
- No account connected → ORDERS column + avg marker simply empty; market depth +
  VAP still render (public data).
- Grouping change → rebuild immediately from the last book (VAP re-bucketed from
  the accumulator at the new grouping).
- Very wide/!na prices → clamp the visible window to `rows_each_side`.

---

## Phase boundary & safety (critical)

Phase 1 is **display only**. Click-to-trade is a **separate future spec** and,
when built, MUST:
- Route every order through the hardened live-trading gates
  (`allow_trading` / `live_trading_armed` / `kill_switch`, paper-vs-live mode)
  and a confirmation / explicit armed state — a fat-finger on a ladder is real
  money.
- Default to paper/preview.

This spec deliberately ships no order-mutation code, so a review can confirm the
ladder cannot place or cancel an order.

---

## Testing

Pure `CryptoLadderModel` unit tests (`tests/tst_crypto_ladder_model.cpp`,
APPLESS, neuter-verified):
- **Bucketing:** raw levels fold into the correct bucket (`bucket_price`), sizes
  within a bucket sum, bids-below-mid / asks-above-mid partitioning.
- **VAP:** trades accumulate per bucket; `reset_vap()` clears; `max_vap` correct;
  a buy and a sell at the same bucket both count toward volume.
- **Row window:** `build` returns rows centered on mid, `rows_each_side` each
  way, ordered top→bottom by descending price.
- **Overlays:** a `MyOrder` lands in the correct bucket's `my_bid_qty`/`my_ask_qty`;
  the avg-entry marker sets `is_avg_entry` on the right bucket.
- **Edge:** empty book → empty rows; grouping change re-buckets.

The `CryptoLadder` widget (painting, dropdown, recenter interaction) is
manually verified, consistent with how `CryptoOrderBook` is tested.

---

## Files

- Create: `src/screens/crypto_trading/CryptoLadderModel.h` (pure)
- Create: `src/screens/crypto_trading/CryptoLadder.{h,cpp}` (widget)
- Create: `tests/tst_crypto_ladder_model.cpp` + register in `tests/CMakeLists.txt`
- Modify: crypto trading screen layout / `LayoutCatalog` wiring to expose the
  ladder as a dockable panel (exact seam identified during planning — follow the
  existing pattern for how `CryptoOrderBook` / `CryptoDepthChart` panels are
  registered).
- Modify: `src/CMakeLists.txt` (or the GUI source list) to compile the new
  widget.

## Out of scope (explicit)

- Click-to-trade, order placement/cancel, `UnifiedTrading` wiring, `Cxl
  buy/sell/all` shortcuts, quantity calculator, Attach-OSO — all a later phase.
- Rolling-window / history-backfilled VAP (session only for now).
- Exchange tick-size metadata fetching (grouping dropdown replaces it).
- Changes to `CryptoOrderBook` (the ladder is additive).
