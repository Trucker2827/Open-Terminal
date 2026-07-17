# AI live path — NOT FOR ARMING until real-account validated

The AI strategy loop's `--mode live` path (PR #39) is **code-hardened and mock-verified, but has NOT been validated against a real brokerage account.** It ships **disarmed**: reaching a live order requires a human GUI arm (`cli_live_armed`, which the AI provably cannot set) AND a per-run `--mode live`, with `submit_order` re-checking every gate (armed → allowed-account → daily-loss → reserve → place_order) as the sole authority. No automated test ever arms a real order.

## Blockers to resolve BEFORE arming against a real broker
1. **Submitted-but-unexecuted CLOSE under-counts the live position cap (Important).** `reconcile_and_record` writes the full submitted qty to the live ledger at submission time, before execution. An accepted-but-unexecuted SELL records `record_close(full qty)`, shrinking the position the per-handler position cap reads next tick — so a later buy can pass the cap while the close is still open; if the close then cancels/expires and the buy fills, real exposure can exceed the cap. Fix: record the live ledger only for **actually-filled** qty (broker-confirmed), and/or re-sync positions from broker truth each tick. (Buys are conservative/safe; only the close direction under-counts.)
2. **Broker status-string contract.** The live-fill detection compares the broker's status to `kLiveFilledStatus` (`"filled"`). A real broker adapter MUST normalize its order status to exactly that token, or a real fill is mis-classified as `live_submitted` (fails safe — under-counts — but the loop won't book the fill).
3. **Closed-loop fill→ledger→cap behavior is inspection-verified, not exercised.** The mock tests seed the live ledger directly; no test drives submit_order → reconcile_and_record → next-tick cap as a closed loop. Validate against a real (or realistic broker-sim) account.
4. **Live short positions.** `LivePosition.qty` is an unsigned magnitude; the live ledger is long-only by construction. If live shorting is ever added, the position cap's signed-net assumption must be revisited.

Until these are resolved, keep the path disarmed.
