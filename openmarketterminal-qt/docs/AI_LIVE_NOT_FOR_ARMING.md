# AI live path — NOT FOR ARMING until real-account validated

The AI strategy loop's `--mode live` path (PR #39) is **code-hardened and mock-verified, but has NOT been validated against a real brokerage account.** It ships **disarmed**: reaching a live order requires a human GUI arm (`cli_live_armed`, which the AI provably cannot set) AND a per-run `--mode live`, with `submit_order` re-checking every gate (armed → allowed-account → daily-loss → reserve → place_order) as the sole authority. No automated test ever arms a real order.

## Current safety boundary

The originally reported submitted-but-unexecuted cap failure is fixed in the
follow-up to #39: the strategy runner rebuilds live exposure from authenticated
broker positions and remaining working orders every tick. It fails closed when
either broker query fails. A new buy counts current position plus pending buys;
a new sell counts current position minus pending sells. Opposite-side working
orders are never trusted as a hedge because they can cancel. Same-tick accepted
orders are reserved immediately, and `reconcile_and_record` writes the audit
ledger only for broker-confirmed filled quantity.

## Blockers to resolve BEFORE arming against a real broker
1. **Persistent fill reconciliation.** The immediate order query is deliberately
   conservative: an accepted/open order does not alter the audit ledger until a
   confirmed fill is observed. A durable user-order/fill stream or periodic
   reconciliation job is still needed to record fills that arrive after that
   one-shot lookup, and to make P&L reporting complete.
2. **Real broker contract validation.** Mock tests cover accepted-unfilled,
   partial fills, canceled opposing orders, same-tick duplicate intent, and
   broker-read failure. They do not prove a real broker's raw position/order
   schemas, status values, partial-fill behavior, or eventual consistency.
   Validate this closed loop against a realistic broker simulator, then a
   deliberately tiny real-account test with the kill switch available.
3. **Live short positions.** Exposure reads now preserve signed broker
   positions, but the audit ledger's `LivePosition` record remains oriented to
   long cost basis. Before live shorting is enabled, extend the P&L/accounting
   representation and validate cover/borrow behavior for the broker.

Until these are resolved, keep the path disarmed.
