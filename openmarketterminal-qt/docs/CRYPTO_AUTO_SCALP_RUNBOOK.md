# Crypto Auto Scalp v1

Crypto Auto Scalp is shadow-only by default. It observes Coinbase and Kraken
simultaneously, forms signals only from completed one-second bars, journals an
immutable opportunity, and records deterministic venue proposals after entry
fee, taker exit fee, spread, slippage, and safety cost.

It does not reproduce promotional chart labels. A BUY or SELL requires a
confirmed closed-bar break of a confirmed pivot. The newest forming bar is
always excluded, so a later tick cannot repaint an earlier signal.

## Start the unattended shadow loop

The persistent OpenTerminal daemon owns the loop and makes it restart-safe:

```sh
./build/openterminalcli --headless daemon scalp start BTC-USD \
  --sources coinbase,kraken,gemini \
  --cadence-ms 250 --taker --max-age-ms 1000 \
  --min-profit-bps 10 --min-live-sources 2
```

Inspect its heartbeat and the latest paired proposal:

```sh
./build/openterminalcli --headless --json daemon scalp status
./build/openterminalcli --headless daemon scalp tape BTC-USD --limit 20
```

Each `crypto-auto-scalp-v1` decision contains:

- a stable `opportunity_id`;
- the non-repainting `reversal_signal`;
- Coinbase Advanced and Kraken Pro `venue_proposals`;
- recorded cost assumptions and blockers;
- one selected proposal, or no selection when every venue is fee-dead/stale;
- `execution_authority: shadow_only`.

## Qualification

Generate the frozen, versioned out-of-sample report:

```sh
./build/openterminalcli --headless daemon scalp qualification
```

The `crypto-scalp-qualification-v1` gate requires all of:

- journal integrity;
- at least 200 resolved opportunities;
- at least 80% resolution coverage;
- positive mean net basis points after the proposal's recorded costs;
- a deterministic bootstrap 95% confidence interval whose lower bound is
  above zero.

The report is written atomically to the profile daemon directory. A mutated
opportunity invalidates the epoch.

## Canary

Canary arming is rejected until the frozen report says `QUALIFIED`. It is
separate from the legacy live arm and still requires every global GUI/CLI
security gate:

```sh
./build/openterminalcli --headless automation arm-scalp-canary \
  --venue kraken --symbols BTC-USD --max-order-usd 5 \
  --max-daily-orders 3 --expires-min 15 \
  --yes --i-understand-live-risk
```

Dry-run the exact proposal-to-order transformation:

```sh
./build/openterminalcli --headless automation execute-next \
  --mode scalp --symbol BTC-USD --dry-run
```

Live execution additionally requires `--yes`, an unexpired canary arm, the
global kill switch off, live and fast-live gates armed, and the venue
allowlisted. Qualification is rechecked immediately before submission.

For Kraken, an authenticated native Spot WebSocket v2 adapter preserves the
proposal's IOC/FOK/GTC, client order id, and 500–5000 ms matching deadline.
For Coinbase Advanced, orders use its authenticated REST order endpoint and
the user WebSocket remains the order/fill update source. Every response records
adapter and order-ack latency. A SELL fails closed unless free spot inventory
can be confirmed; this system does not open margin shorts.

Never lower the qualification thresholds to make the canary turn green.
