# Secure Trading MCP Server: Alpaca + Coinbase + Kraken

A paper-default MCP server exposing high-level tools for Alpaca Trading API, Coinbase Advanced Trade, and Kraken Spot. It is intentionally conservative: dry-run is enabled by default, live trading requires explicit environment opt-in plus per-call confirmation, risk limits are enforced before any order call, and crypto withdrawals are disabled unless explicitly enabled.

## Features

- MCP server using the official Python MCP SDK `FastMCP`.
- Alpaca tools: account, portfolio, market data, historical data, orders, cancel, open orders, simple position analysis, simple backtest.
- Coinbase tools: accounts, product/market data, orders, cancel, open orders, simple crypto market data, guarded `coinbase_send_crypto` stub.
- Kraken tools: balances, ticker/market data, open orders, cancel, and spot limit post-only orders.
- Paper/live mode via environment variables.
- API keys loaded from `.env`; never hardcoded.
- Rate limiting, structured errors, audit logging, and notional/risk limits.
- stdio for Claude Desktop/Code; Streamable HTTP/SSE-style remote deployment depending on MCP SDK version.

## Install

```bash
python3.11 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
$EDITOR .env
```

Start paper/dry-run first:

```bash
TRADING_MODE=paper DRY_RUN=true python server.py
```

## Run locally with stdio

```bash
python server.py --transport stdio
```

## Run remotely over HTTP

```bash
python server.py --transport streamable-http --host 127.0.0.1 --port 8765
```

Put this behind HTTPS, authentication, and an IP allowlist before exposing beyond localhost. Do not expose a live-trading MCP server directly to the public internet.

## Claude Desktop config

macOS config path is usually:

```text
~/Library/Application Support/Claude/claude_desktop_config.json
```

Example:

```json
{
  "mcpServers": {
    "secure-trading": {
      "command": "/absolute/path/to/trading-mcp-server/.venv/bin/python",
      "args": ["/absolute/path/to/trading-mcp-server/server.py", "--transport", "stdio"],
      "env": {
        "TRADING_MODE": "paper",
        "DRY_RUN": "true"
      }
    }
  }
}
```

## Safety model

To place any real order:

1. `TRADING_MODE=live`
2. `DRY_RUN=false`
3. `REQUIRE_CONFIRMATION=true`
4. Tool call includes `confirmation_token="I_UNDERSTAND_LIVE_TRADE"` or your configured token.
5. Order must pass max notional, max daily count, and asset-specific gates.

Options trading additionally requires `ALLOW_OPTIONS_TRADING=true`. Crypto withdrawals require `ALLOW_CRYPTO_WITHDRAWALS=true`, but this project leaves withdrawal implementation disabled by default because Advanced Trade trading keys and withdrawal/payment permissions have very different risk profiles.

Coinbase and Kraken each have their own execution arm in addition to global live mode:

- Coinbase live orders require `COINBASE_ALLOW_TRADING=true` and `COINBASE_ALLOWED_SYMBOLS`.
- Kraken live orders require `KRAKEN_ALLOW_TRADING=true` and `KRAKEN_ALLOWED_SYMBOLS`.
- Kraken orders are forced to spot `limit` + `post-only`; market/margin/leverage orders are not sent.

## Test

```bash
python tests_smoke.py                       # RiskManager logic + backtest
python -m unittest test_place_order -v       # the place_order TOOL path (no keys/network)
```

The smoke test does not touch broker APIs. It verifies config parsing, dry-run behavior, risk blocking, and the simple backtest function.

`test_place_order.py` drives the real `place_order` flow (stubbing only the SDK price/position lookups) and locks four behaviors on both venues: within-cap dry-run preview, order-cap block, position-cap block + sell exemption, and fail-closed market-order-with-no-price. Neuter-verified (disabling the fail-closed guard fails the market-no-price cases).

## Verification status (read before going live)

This fork hardened the safety layer. What is and isn't verified:

**Verified (tested / run):**
- `safety.check_trade` is **fail-closed**: a sized order with no determinable price is **blocked**, never allowed to skip `MAX_ORDER_NOTIONAL_USD` (the old market-order bypass). Covered by `tests_smoke.py` + neuter-checked.
- `MAX_POSITION_NOTIONAL_USD` is enforced on BUYs (was previously dead). Covered by the smoke test.
- OCC options → 100× notional. Server starts and all 10 tools register on Python **3.11 and 3.14**.
- Network transport (sse/streamable-http) is refused unless `TRADING_MCP_ALLOW_HTTP=true` **and** the host is loopback. stdio is the default.

**NOT yet verified (needs a one-time live paper smoke — no API keys in this environment):**
- The SDK **price** lookups (`_estimate_price`) and **position** lookups (`_current_position_notional`) on both venues are written against the official alpaca-py / coinbase-advanced-py APIs but have **not been executed**. Failure modes:
  - If a price lookup's shape is wrong → it returns `None` → the order **fails closed (blocked)**. Safe, but market orders would then be unusable until fixed.
  - If a position lookup's shape is wrong → it returns `None` → the position cap is **skipped (fails open)** for that order. The per-order notional cap still applies.

**Discriminating test (run this with paper keys before trusting market orders):**
1. Put paper Alpaca keys in `.env`, `TRADING_MODE=paper`, `DRY_RUN=true`.
2. Call `place_order` (market) on a liquid symbol (e.g. AAPL). A sized `would_submit` preview ⇒ price plumbing works; a "no price available" block ⇒ the price lookup needs fixing for your SDK version.
3. Buy a symbol you already hold ⇒ confirm `current_position_notional` arrives as a number (position cap live), not `None`.

Requires Python 3.11+ (verified on 3.11 and 3.14).
