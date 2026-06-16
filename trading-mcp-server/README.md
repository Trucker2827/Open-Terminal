# Secure Trading MCP Server: Alpaca + Coinbase

A paper-default MCP server exposing high-level tools for Alpaca Trading API and Coinbase Advanced Trade. It is intentionally conservative: dry-run is enabled by default, live trading requires explicit environment opt-in plus per-call confirmation, risk limits are enforced before any order call, and crypto withdrawals are disabled unless explicitly enabled.

## Features

- MCP server using the official Python MCP SDK `FastMCP`.
- Alpaca tools: account, portfolio, market data, historical data, orders, cancel, open orders, simple position analysis, simple backtest.
- Coinbase tools: accounts, product/market data, orders, cancel, open orders, simple crypto market data, guarded `coinbase_send_crypto` stub.
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

## Test

```bash
python tests_smoke.py
```

The smoke test does not touch broker APIs. It verifies config parsing, dry-run behavior, risk blocking, and the simple backtest function.
