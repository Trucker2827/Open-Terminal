# Crypto Account State + Gated Execution — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the AI/CLI read-only visibility into the Coinbase account (balances, open orders, trades) plus gated real-money order execution, reusing the existing trading-constitution gates — without touching the Alpaca/`IBroker` path.

**Architecture:** New `crypto_*` MCP tools in `CryptoTradingTools.cpp` route to the existing `ExchangeService` (ccxt daemon). Read tools are non-destructive/ungated (same posture as `get_ticker`). The two execution tools are added to `SettingsGate::is_fast_live_tool()` so all three AI auth-checkers gate them with `allow_trading && live_armed && fast_live_armed`; their handlers re-enforce kill-switch → arms → venue → risk-floor before calling the exchange and recording to `trade_audit`.

**Tech Stack:** C++20/Qt6, CMake+Ninja unity build, QtTest. ccxt via the Python exchange daemon behind `ExchangeService`.

**Spec:** `docs/design/2026-06-16-crypto-account-execution-design.md`

---

## Build / test commands
- Tests build: `cmake --build /tmp/ot-build-test --target <test_target>` (configure once: `cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew`)
- GUI/CLI build: `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`
- Run a test: `/tmp/ot-build-test/tests/<test_target>`

## File structure (what changes)
- **Modify** `src/mcp/tools/SettingsGate.cpp` — add 2 names to `is_fast_live_tool()`.
- **Modify** `src/mcp/tools/CryptoTradingTools.cpp` — add 3 read tools + 2 exec tools + `crypto_risk_verdict` + local `crypto_read_cap`.
- **Create** `src/trading/CryptoRisk.{h,cpp}` — the pure `crypto_risk_verdict` (so it is unit-testable without Qt-widget/DB deps, mirroring `OptionSymbol`).
- **Modify** `CMakeLists.txt` — add `CryptoRisk.cpp` to `openterminal_core` sources (next to `OptionSymbol.cpp`).
- **Create** `tests/tst_crypto_risk.cpp` + register in `tests/CMakeLists.txt`.
- **Modify** `tests/tst_settings_gate.cpp` — gate-routing asserts for the 2 exec tools.
- **No change** to Alpaca, `IBroker`, `AccountManager`, the broker order tools.

---

### Task 0: Branch

- [ ] **Step 1: Create the feature branch off main**

```bash
cd ~/src/Open-Terminal/openmarketterminal-qt
git checkout -b feat/crypto-account-execution
git rev-parse --abbrev-ref HEAD   # expect: feat/crypto-account-execution
```

---

### Task 1: Pure risk verdict (`crypto_risk_verdict`) — TDD

**Files:**
- Create: `src/trading/CryptoRisk.h`, `src/trading/CryptoRisk.cpp`
- Modify: `CMakeLists.txt` (add `src/trading/CryptoRisk.cpp` to the core lib sources, near `src/trading/options/OptionSymbol.cpp`)
- Test: `tests/tst_crypto_risk.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/tst_crypto_risk.cpp`

```cpp
// tst_crypto_risk.cpp — pure crypto order-notional risk verdict.
#include <QtTest>
#include "trading/CryptoRisk.h"

using namespace openmarketterminal::trading;

class TstCryptoRisk : public QObject {
    Q_OBJECT
  private slots:
    void order_value_is_qty_times_price() {
        QCOMPARE(crypto_risk_verdict(0.001, 60000.0, 500.0).order_value, 60.0);
        QCOMPARE(crypto_risk_verdict(2.0, 1.21, 500.0).order_value, 2.42);
    }
    void under_cap_passes() {
        const auto v = crypto_risk_verdict(0.001, 60000.0, 500.0); // $60 <= $500
        QVERIFY(v.ok);
        QVERIFY(v.reason.isEmpty());
    }
    void over_cap_rejected_with_reason() {
        const auto v = crypto_risk_verdict(0.02, 60000.0, 500.0);   // $1200 > $500
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains("max order value"));
    }
    void zero_price_rejected() {
        const auto v = crypto_risk_verdict(1.0, 0.0, 500.0);
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains("no price"));
    }
    void zero_or_negative_qty_rejected() {
        QVERIFY(!crypto_risk_verdict(0.0, 60000.0, 500.0).ok);
        QVERIFY(!crypto_risk_verdict(-1.0, 60000.0, 500.0).ok);
    }
};
QTEST_MAIN(TstCryptoRisk)
#include "tst_crypto_risk.moc"
```

- [ ] **Step 2: Register the test** — append to `tests/CMakeLists.txt` (mirror `tst_option_symbol`)

```cmake
add_executable(tst_crypto_risk tst_crypto_risk.cpp)
target_include_directories(tst_crypto_risk PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_risk PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_risk COMMAND tst_crypto_risk)
```

- [ ] **Step 3: Run it to confirm it fails to build/link** (header doesn't exist yet)

```bash
cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew >/dev/null
cmake --build /tmp/ot-build-test --target tst_crypto_risk 2>&1 | tail -5
# Expected: FAIL — 'trading/CryptoRisk.h' file not found
```

- [ ] **Step 4: Write the header** — `src/trading/CryptoRisk.h`

```cpp
#pragma once
// CryptoRisk.h — pure notional risk verdict for crypto (spot) orders.
// No Qt-widget/DB deps so it is unit-testable in isolation (mirrors OptionSymbol).
#include <QString>

namespace openmarketterminal::trading {

struct CryptoRiskVerdict {
    bool ok = false;
    double order_value = 0.0;   // quantity * resolved_price
    QString reason;             // empty when ok; human-readable rejection otherwise
};

// order_value = quantity * resolved_price (spot → multiplier 1). Rejects on
// non-positive quantity, non-positive price ("no price available"), or
// order_value > max_order_value ("exceeds max order value").
CryptoRiskVerdict crypto_risk_verdict(double quantity, double resolved_price, double max_order_value);

} // namespace openmarketterminal::trading
```

- [ ] **Step 5: Write the implementation** — `src/trading/CryptoRisk.cpp`

```cpp
#include "trading/CryptoRisk.h"

namespace openmarketterminal::trading {

CryptoRiskVerdict crypto_risk_verdict(double quantity, double resolved_price, double max_order_value) {
    CryptoRiskVerdict v;
    if (quantity <= 0.0) {
        v.reason = QStringLiteral("quantity must be > 0");
        return v;
    }
    if (resolved_price <= 0.0) {
        v.reason = QStringLiteral("no price available");
        return v;
    }
    v.order_value = quantity * resolved_price; // spot: contract multiplier = 1
    if (v.order_value > max_order_value) {
        v.reason = QStringLiteral("order value %1 exceeds max order value %2")
                       .arg(v.order_value, 0, 'f', 2).arg(max_order_value, 0, 'f', 2);
        return v;
    }
    v.ok = true;
    return v;
}

} // namespace openmarketterminal::trading
```

- [ ] **Step 6: Add `CryptoRisk.cpp` to the core lib** — in `CMakeLists.txt`, find the line adding `src/trading/options/OptionSymbol.cpp` to the core target's sources and add `src/trading/CryptoRisk.cpp` immediately after it.

```bash
grep -n "trading/options/OptionSymbol.cpp" CMakeLists.txt   # locate the sources list
# add:    src/trading/CryptoRisk.cpp
```

- [ ] **Step 7: Build + run the test → PASS**

```bash
cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew >/dev/null
cmake --build /tmp/ot-build-test --target tst_crypto_risk 2>&1 | tail -3
/tmp/ot-build-test/tests/tst_crypto_risk
# Expected: Totals: 5 passed, 0 failed
```

- [ ] **Step 8: Neuter-check** — temporarily change `>` to `<` in the cap comparison, rebuild, confirm `over_cap_rejected_with_reason` FAILS, then revert.

- [ ] **Step 9: Commit**

```bash
git add src/trading/CryptoRisk.h src/trading/CryptoRisk.cpp CMakeLists.txt tests/tst_crypto_risk.cpp tests/CMakeLists.txt
git commit -m "feat(crypto): pure crypto_risk_verdict (order notional vs max_order_value)"
```

---

### Task 2: Gate routing — register exec tools as fast-live — TDD

**Files:**
- Modify: `src/mcp/tools/SettingsGate.cpp:87-99` (`is_fast_live_tool`)
- Test: `tests/tst_settings_gate.cpp`

- [ ] **Step 1: Add the failing routing test** — add this slot to the test class in `tests/tst_settings_gate.cpp`

```cpp
void crypto_exec_tools_route_to_fast_live_gate() {
    // The two crypto execution tools must be gated as fast-live (arms required),
    // and must NOT be on the raw live_* deny-list.
    QVERIFY(mcp::is_fast_live_tool(QStringLiteral("crypto_submit_order")));
    QVERIFY(mcp::is_fast_live_tool(QStringLiteral("crypto_cancel_order")));
    QVERIFY(!mcp::is_live_execution_tool(QStringLiteral("crypto_submit_order")));
    QVERIFY(!mcp::is_live_execution_tool(QStringLiteral("crypto_cancel_order")));
}
```

- [ ] **Step 2: Run it to confirm it fails**

```bash
cmake --build /tmp/ot-build-test --target tst_settings_gate 2>&1 | tail -3
/tmp/ot-build-test/tests/tst_settings_gate 2>&1 | grep -i "crypto_exec_tools"
# Expected: FAIL on the first is_fast_live_tool assert
```

- [ ] **Step 3: Add the two names** — in `src/mcp/tools/SettingsGate.cpp`, the `is_fast_live_tool` set literal (currently includes `fast_submit_order`, `cancel_order`, `replace_order`, `exit_position`, `get_positions`, `get_open_orders`, `get_fills`). Add:

```cpp
        QStringLiteral("crypto_submit_order"), QStringLiteral("crypto_cancel_order"),
```

- [ ] **Step 4: Build + run → PASS**

```bash
cmake --build /tmp/ot-build-test --target tst_settings_gate 2>&1 | tail -3
/tmp/ot-build-test/tests/tst_settings_gate
# Expected: all pass, including crypto_exec_tools_route_to_fast_live_gate
```

- [ ] **Step 5: Commit**

```bash
git add src/mcp/tools/SettingsGate.cpp tests/tst_settings_gate.cpp
git commit -m "feat(crypto): gate crypto_submit_order/cancel_order as fast-live (arms required)"
```

---

### Task 3: Read-only account tools (visibility) — build-verified + live integration

**Files:**
- Modify: `src/mcp/tools/CryptoTradingTools.cpp` (add 3 tools inside `get_crypto_trading_tools()`)

These hit the live exchange via `ExchangeService` (no offline unit test, exactly like `get_ticker`). Verify by build + a real read-only CLI call.

- [ ] **Step 1: Add the three read tools** — insert after the existing `get_exchange_info` block, before `return tools;`. Mirror the `get_ticker` structure.

```cpp
    // ── get_crypto_balance ─────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_balance";
        t.description = "Get account balances (per-currency free/used/total) from the configured crypto exchange.";
        t.category = "crypto-trading";
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_balance();
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_crypto_open_orders ─────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_open_orders";
        t.description = "Get open orders on the configured crypto exchange (optionally filtered by symbol).";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Optional pair filter (e.g. BTC/USD)"}}}};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_open_orders_live(args.value("symbol").toString().trimmed());
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_crypto_trades ──────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_trades";
        t.description = "Get recent personal fills for a symbol on the configured crypto exchange.";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Trading pair (e.g. BTC/USD)"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max fills (default 50, cap 200)"}}}};
        t.input_schema.required = {"symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString symbol = args.value("symbol").toString().trimmed();
            if (symbol.isEmpty())
                return ToolResult::fail("Missing 'symbol'");
            int limit = args.value("limit").toInt(50);
            if (limit <= 0 || limit > 200) limit = 50;
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_my_trades(symbol, limit);
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }
```

- [ ] **Step 2: Build the GUI/CLI**

```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli 2>&1 | tail -3
```

- [ ] **Step 3: Live read-only verification** (app must be running + Coinbase connected; this is read-only, safe)

```bash
/tmp/ot-build-ht/openterminalcli mcp call get_crypto_balance '{}'
# Expected: {"data":{"balances":{...}},"success":true} with the real currencies
```

- [ ] **Step 4: Commit**

```bash
git add src/mcp/tools/CryptoTradingTools.cpp
git commit -m "feat(crypto): read-only account tools (balance/open orders/trades) for AI/CLI"
```

---

### Task 4: Gated execution tools (`crypto_submit_order` / `crypto_cancel_order`)

**Files:**
- Modify: `src/mcp/tools/CryptoTradingTools.cpp` (add includes, a file-local `crypto_read_cap` + `crypto_exec_audit`, and the 2 exec tools)

- [ ] **Step 1: Add includes + file-local helpers** — at the top of `CryptoTradingTools.cpp` add includes and, inside the `openmarketterminal::mcp::tools` namespace above `get_crypto_trading_tools()`, the helpers.

```cpp
#include "mcp/tools/SettingsGate.h"
#include "mcp/tools/LivePnl.h"
#include "trading/CryptoRisk.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include <QDateTime>
#include <QJsonDocument>
```

```cpp
namespace {
constexpr const char* kCryptoTag = "CryptoTradingTools";

// Mirrors FastLiveTools::read_cap (anon-ns there, not exported).
double crypto_read_cap(const QString& key, double default_val) {
    auto r = storage::SettingsRepository::instance().get(key, QString());
    if (!r.is_ok()) return default_val;
    bool ok = false;
    const double v = r.value().toDouble(&ok);
    return (!ok || v <= 0.0) ? default_val : v;
}

// Mirrors FastLiveTools::fast_derisk_audit but phase="crypto-live".
void crypto_exec_audit(const QString& tool, const QString& venue, const QString& decision,
                       const QString& reason, const QJsonObject& intent) {
    storage::TradeAuditRow row;
    row.ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    row.phase = QStringLiteral("crypto-live");
    row.tool = tool;
    row.account = venue;
    row.mode = QStringLiteral("live");
    row.intent_json = QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    row.decision = decision;
    row.reason = reason;
    row.risk_snapshot_json = QStringLiteral("{}");
    auto r = storage::TradeAuditRepository::instance().append(row);
    if (!r.is_ok())
        LOG_WARN(kCryptoTag, "crypto trade_audit append failed: " + QString::fromStdString(r.error()));
}
} // namespace
```

> Note: confirm the exact namespaces of `SettingsRepository`/`TradeAuditRepository`/`TradeAuditRow` by checking their headers (`storage::` is used here per `FastLiveTools.cpp` includes — match whatever that file uses). `daily_loss_ok` is `openmarketterminal::mcp::tools::daily_loss_ok` (same namespace as this file, no qualifier needed).

- [ ] **Step 2: Add `crypto_submit_order`** — inside `get_crypto_trading_tools()`, after the read tools.

```cpp
    // ── crypto_submit_order (GATED, real-money) ────────────────────────
    {
        ToolDef t;
        t.name = "crypto_submit_order";
        t.description = "Place a spot order on the configured crypto exchange (market or limit). "
                        "Gated: requires the live arms; enforces kill-switch, allowed-venue, and the "
                        "deterministic risk floor (cli.risk.max_order_value / max_daily_loss).";
        t.category = "crypto-trading";
        t.is_destructive = true;
        t.auth_required = AuthLevel::Authenticated;
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Pair (e.g. BTC/USD)"}}},
            {"side", QJsonObject{{"type", "string"}, {"description", "buy or sell"}}},
            {"quantity", QJsonObject{{"type", "number"}, {"description", "Base-asset amount (> 0)"}}},
            {"order_type", QJsonObject{{"type", "string"}, {"description", "market or limit (default market)"}}},
            {"limit_price", QJsonObject{{"type", "number"}, {"description", "Required for limit orders"}}}};
        t.input_schema.required = {"symbol", "side", "quantity"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString symbol = args.value("symbol").toString().trimmed();
            const QString side = args.value("side").toString().trimmed().toLower();
            const double quantity = args.value("quantity").toDouble(0.0);
            const QString otype = args.value("order_type").toString("market").trimmed().toLower();
            auto& svc = trading::ExchangeService::instance();
            const QString venue = svc.get_exchange();
            QJsonObject intent{{"symbol", symbol}, {"side", side}, {"quantity", quantity},
                               {"order_type", otype}};
            if (args.contains("limit_price")) intent["limit_price"] = args.value("limit_price").toDouble();

            // ── Gate sequence (defense-in-depth; the auth-checker already
            //    required the arms, but the handler never trusts the router). ──
            if (mcp::cli_kill_switch_engaged()) {
                crypto_exec_audit(t.name, venue, "denied", "kill switch engaged", intent);
                return ToolResult::fail("Refused: kill switch engaged");
            }
            if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed())) {
                crypto_exec_audit(t.name, venue, "denied", "not armed", intent);
                return ToolResult::fail("Refused: live trading not armed");
            }
            if (venue.isEmpty()) {
                crypto_exec_audit(t.name, venue, "denied", "no exchange configured", intent);
                return ToolResult::fail("No exchange configured");
            }
            if (!mcp::cli_venue_allowed(venue)) {
                crypto_exec_audit(t.name, venue, "denied", "venue not allowed: " + venue, intent);
                return ToolResult::fail("Refused: venue not allowed (" + venue + ")");
            }
            if (side != "buy" && side != "sell") {
                crypto_exec_audit(t.name, venue, "denied", "invalid side", intent);
                return ToolResult::fail("side must be buy or sell");
            }

            // ── Resolve price for the risk floor: limit→limit_price; market→cache→fetch. ──
            double price = 0.0;
            if (otype == "limit") {
                price = args.value("limit_price").toDouble(0.0);
            } else {
                price = svc.get_cached_price(symbol).last;
                if (price <= 0.0) {
                    try { price = svc.fetch_ticker(symbol).last; } catch (...) { price = 0.0; }
                }
            }
            const double cap = crypto_read_cap(QStringLiteral("cli.risk.max_order_value"), 25000.0);
            const auto rv = trading::crypto_risk_verdict(quantity, price, cap);
            if (!rv.ok) {
                crypto_exec_audit(t.name, venue, "denied", rv.reason, intent);
                return ToolResult::fail("Risk floor: " + rv.reason);
            }
            if (!daily_loss_ok(rv.order_value)) {
                crypto_exec_audit(t.name, venue, "denied", "daily loss cap", intent);
                return ToolResult::fail("Risk floor: daily loss cap would be exceeded");
            }

            // ── Execute ──
            try {
                const double limit_px = (otype == "limit") ? price : 0.0;
                const QJsonObject res = svc.place_exchange_order(symbol, side, otype, quantity, limit_px);
                if (res.contains("error") || (res.contains("success") && !res.value("success").toBool())) {
                    const QString msg = res.value("error").toString(res.value("message").toString("exchange error"));
                    crypto_exec_audit(t.name, venue, "rejected", msg, intent);
                    return ToolResult::fail("Exchange rejected order: " + msg);
                }
                const QString status = res.value("status").toString(res.value("data").toObject().value("status").toString("submitted"));
                crypto_exec_audit(t.name, venue, status.isEmpty() ? "submitted" : status, "", intent);
                return ToolResult::ok_data(res);
            } catch (const std::exception& e) {
                crypto_exec_audit(t.name, venue, "rejected", e.what(), intent);
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }
```

- [ ] **Step 3: Add `crypto_cancel_order`** — after `crypto_submit_order`.

```cpp
    // ── crypto_cancel_order (GATED) ────────────────────────────────────
    {
        ToolDef t;
        t.name = "crypto_cancel_order";
        t.description = "Cancel an open order on the configured crypto exchange. Gated: requires the live arms.";
        t.category = "crypto-trading";
        t.is_destructive = true;
        t.auth_required = AuthLevel::Authenticated;
        t.input_schema.properties = QJsonObject{
            {"order_id", QJsonObject{{"type", "string"}, {"description", "Exchange order id"}}},
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Pair the order is on (e.g. BTC/USD)"}}}};
        t.input_schema.required = {"order_id", "symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString order_id = args.value("order_id").toString().trimmed();
            const QString symbol = args.value("symbol").toString().trimmed();
            auto& svc = trading::ExchangeService::instance();
            const QString venue = svc.get_exchange();
            QJsonObject intent{{"order_id", order_id}, {"symbol", symbol}};
            if (mcp::cli_kill_switch_engaged()) {
                crypto_exec_audit(t.name, venue, "denied", "kill switch engaged", intent);
                return ToolResult::fail("Refused: kill switch engaged");
            }
            if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed())) {
                crypto_exec_audit(t.name, venue, "denied", "not armed", intent);
                return ToolResult::fail("Refused: live trading not armed");
            }
            if (venue.isEmpty() || !mcp::cli_venue_allowed(venue)) {
                crypto_exec_audit(t.name, venue, "denied", "venue not allowed", intent);
                return ToolResult::fail("Refused: venue not allowed");
            }
            if (order_id.isEmpty() || symbol.isEmpty())
                return ToolResult::fail("order_id and symbol are required");
            try {
                const QJsonObject res = svc.cancel_exchange_order(order_id, symbol);
                if (res.contains("error")) {
                    crypto_exec_audit(t.name, venue, "rejected", res.value("error").toString(), intent);
                    return ToolResult::fail("Cancel failed: " + res.value("error").toString());
                }
                crypto_exec_audit(t.name, venue, "cancelled", "", intent);
                return ToolResult::ok_data(res);
            } catch (const std::exception& e) {
                crypto_exec_audit(t.name, venue, "rejected", e.what(), intent);
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }
```

- [ ] **Step 4: Build GUI/CLI**

```bash
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli 2>&1 | tail -3
# Expected: clean (only the usual ld SDK-version warnings)
```

- [ ] **Step 5: Verify the gate refuses when kill switch is engaged** (no real order placed)

This is the safety-critical manual check. With the app running + armed (current state), engage the kill switch in GUI Settings → Security, then:

```bash
/tmp/ot-build-ht/openterminalcli mcp call crypto_submit_order \
  '{"symbol":"BTC/USD","side":"buy","quantity":0.0001,"order_type":"limit","limit_price":1000}'
# Expected: tool error "Refused: kill switch engaged"; a trade_audit row decision=denied.
```
Then disengage the kill switch.

- [ ] **Step 6: Live round-trip with a LOWERED cap (real money — operator-driven, tiny)**

> The operator (human) must first set `cli.risk.max_order_value` low (e.g. 5) in GUI Settings and choose a far-from-market limit so it cannot fill. This step places a REAL Coinbase order; keep size minimal and cancel immediately.

```bash
# Far-from-market BTC buy limit, tiny size, value well under the lowered cap:
/tmp/ot-build-ht/openterminalcli mcp call crypto_submit_order \
  '{"symbol":"BTC/USD","side":"buy","quantity":0.00005,"order_type":"limit","limit_price":40000}'
# Expected: {"success":true,...,"status":"open"/"new"}; capture order_id.
/tmp/ot-build-ht/openterminalcli mcp call get_crypto_open_orders '{"symbol":"BTC/USD"}'   # see it resting
/tmp/ot-build-ht/openterminalcli mcp call crypto_cancel_order '{"order_id":"<id>","symbol":"BTC/USD"}'
# Expected: cancelled; trade_audit shows submit + cancel rows; AI-activity toasts fired.
```

- [ ] **Step 7: Commit**

```bash
git add src/mcp/tools/CryptoTradingTools.cpp
git commit -m "feat(crypto): gated crypto_submit_order/cancel_order (kill/arms/venue/risk-floor + audit)"
```

---

### Task 5: Full-suite regression + final review

- [ ] **Step 1: Run the whole test suite**

```bash
cmake --build /tmp/ot-build-test 2>&1 | tail -3
cd /tmp/ot-build-test && ctest --output-on-failure 2>&1 | tail -20; cd -
# Expected: 100% pass, including tst_crypto_risk + tst_settings_gate.
```

- [ ] **Step 2: Confirm the Alpaca path is untouched** — `git diff main --stat` shows no changes under `src/trading/brokers/`, `AccountManager`, `IBroker`, or the fast/order-flow tool files beyond `SettingsGate.cpp`'s 2-name addition.

- [ ] **Step 3: Final adversarial code review** (dispatch a reviewer subagent): focus on the gate sequence ordering (kill-switch FIRST, before any exchange call), that a denied path never reaches `place_exchange_order`, that read tools expose no write capability, and that `is_fast_live_tool` membership actually routes on all three hosts.

---

## Self-Review (author checklist)

**Spec coverage:** (1) read tools → Task 3 ✓; (2) `crypto_risk_floor` → Task 1 (pure verdict) + Task 4 (cap read + daily_loss_ok wiring) ✓; (3) gated exec tools + in-handler gate sequence → Task 4 ✓; (4) `is_fast_live_tool` (not `is_live_execution_tool`) → Task 2 ✓; (5) tests: risk matrix (Task 1), routing asserts (Task 2), kill/arm/venue refusal records denied audit (Task 4 Step 5 manual + the gate sequence emits `crypto_exec_audit("denied",…)`) ✓. **No change to Alpaca/IBroker/AccountManager** → Task 5 Step 2 verifies ✓.

**Placeholder scan:** none — every code step is concrete. The one flagged uncertainty (repository namespace `storage::`) is an explicit "confirm against the header" instruction, not a placeholder.

**Type consistency:** `crypto_risk_verdict(quantity, resolved_price, max_order_value)` signature identical in Task 1 header/impl/test and Task 4 call site. `CryptoRiskVerdict{ok, order_value, reason}` used consistently. Tool names `crypto_submit_order`/`crypto_cancel_order` identical in Tasks 2 and 4.

> **Operator note carried from the spec:** because execution reuses the existing (already-ON) arms, these tools are live the moment Task 4 ships. Lower `cli.risk.max_order_value` before Task 4 Step 6 and keep the first real order tiny.
