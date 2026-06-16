# Alpaca Options v1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make Alpaca options usable + SAFE from the AI/CLI path: size the risk floor correctly for the 100-share contract multiplier, add a contract-discovery tool, and let the validator accept options.

**Architecture:** A pure `option_contract_multiplier(symbol)` (OCC-format detection → 100, else 1) feeds both deterministic risk floors so option exposure is counted ×100. A read-only `get_option_contracts` MCP tool routes to the allowed account's Alpaca adapter (`/v2/options/contracts`). The order path itself is unchanged (already works via symbol pass-through).

**Tech Stack:** Qt6 C++20, QtTest, `QRegularExpression`, `BrokerHttp`, `AccountManager`, the existing risk floors in `src/mcp/tools/`.

**Builds:** test `/tmp/ot-build-test` (`-DOPENMARKETTERMINAL_BUILD_TESTS=ON`), GUI/CLI `/tmp/ot-build-ht`. Branch `feat/alpaca-options` (spec committed there).

**Verification discipline:** tests must RUN and FAIL without the change (neuter-proof). QtTest `QVERIFY/QCOMPARE` only. Confirm the `.o` rebuilt after edits.

---

## File structure

| File | Responsibility | Change | Target |
|---|---|---|---|
| `src/trading/options/OptionSymbol.{h,cpp}` | pure OCC detection + multiplier | **create** | core (tested) |
| `src/mcp/tools/FastLiveTools.cpp` | fast risk floor | apply multiplier (line ~233) | core |
| `src/mcp/tools/OrderFlowTools.cpp` | equity risk floor | apply multiplier (line ~112) | core |
| `src/trading/OrderValidator.cpp` | valid exchanges | add `OPRA` | core |
| `src/trading/BrokerInterface.h` | broker interface | add `get_option_contracts` virtual (default: not supported) | core |
| `src/trading/brokers/alpaca/AlpacaBroker.{h,cpp}` | Alpaca adapter | implement `get_option_contracts` | core |
| `src/mcp/tools/OptionsTools.{cpp,h}` | the discovery MCP tool | **create** | core |
| `src/mcp/McpInit.cpp` | tool registration | register `get_options_tools()` | core |
| `tests/tst_option_symbol.cpp` + `tests/CMakeLists.txt` | pure-helper tests | **create** + register | test |
| `tests/tst_fast_live.cpp` | risk-floor safety-bite test | add | test |
| `CMakeLists.txt` | core sources | add OptionSymbol.cpp + OptionsTools.cpp | — |

---

## Task 1: Pure `option_contract_multiplier` (the safety core)

**Files:** Create `src/trading/options/OptionSymbol.{h,cpp}`; Create `tests/tst_option_symbol.cpp`; Modify `tests/CMakeLists.txt`, `CMakeLists.txt`.

- [ ] **Step 1: Create the header** `src/trading/options/OptionSymbol.h`:
```cpp
#pragma once
#include <QString>

namespace openmarketterminal::trading {

/// True iff `symbol` is an OCC option symbol: root(1-6 A-Z) + YYMMDD + C|P + 8-digit strike.
/// Equity ("AAPL"), crypto ("BTC/USD"), and "EXCH:SYM:CONID" never match.
bool is_occ_option_symbol(const QString& symbol);

/// Shares represented per unit: 100 for an OCC option symbol, else 1.
/// Used by the risk floors so option exposure (≈ price × 100 / contract) is counted correctly.
int option_contract_multiplier(const QString& symbol);

} // namespace openmarketterminal::trading
```

- [ ] **Step 2: Write the failing test** — create `tests/tst_option_symbol.cpp`:
```cpp
#include "trading/options/OptionSymbol.h"
#include <QtTest>

using namespace openmarketterminal::trading;

class TestOptionSymbol : public QObject {
    Q_OBJECT
  private slots:
    void occSymbolsAreOptions();
    void nonOptionsAreOne();
    void malformedIsOne();
};

void TestOptionSymbol::occSymbolsAreOptions() {
    for (const char* s : {"AAPL260821C00110000","SPY261218P00450000","F270115C00012500",
                          "TSLA260116P00250000","A260821C00050000"}) {
        QVERIFY2(is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 100);
    }
}
void TestOptionSymbol::nonOptionsAreOne() {
    for (const char* s : {"AAPL","MSFT","BRK.B","BTC/USD","NASDAQ:AAPL:265598","SPY","V"}) {
        QVERIFY2(!is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 1);
    }
}
void TestOptionSymbol::malformedIsOne() {
    for (const char* s : {"","AAPL26X","AAPL260821X00110000","AAPL26082C00110000",
                          "TOOLONGROOT260821C00110000","aapl260821c00110000"}) {  // lowercase too
        QVERIFY2(!is_occ_option_symbol(s), s);
        QCOMPARE(option_contract_multiplier(s), 1);
    }
}

QTEST_MAIN(TestOptionSymbol)
#include "tst_option_symbol.moc"
```

- [ ] **Step 3: Register the test** — append to `tests/CMakeLists.txt` (mirror `tst_fast_live`):
```cmake
add_executable(tst_option_symbol tst_option_symbol.cpp)
target_include_directories(tst_option_symbol PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_option_symbol PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_option_symbol COMMAND tst_option_symbol)
```

- [ ] **Step 4: Add the .cpp to core** — in `CMakeLists.txt`, add `src/trading/options/OptionSymbol.cpp` to the `openterminal_core` source list (near other `src/trading/*.cpp`, ~line 1057-1110).

- [ ] **Step 5: Build — expect FAIL** (functions undefined):
```
cmake -S . -B /tmp/ot-build-test -G Ninja -DOPENMARKETTERMINAL_BUILD_TESTS=ON >/dev/null
cmake --build /tmp/ot-build-test --target tst_option_symbol -j8
```

- [ ] **Step 6: Implement** `src/trading/options/OptionSymbol.cpp`:
```cpp
#include "trading/options/OptionSymbol.h"
#include <QRegularExpression>

namespace openmarketterminal::trading {

bool is_occ_option_symbol(const QString& symbol) {
    // OCC compact form (as Alpaca returns): root 1-6 uppercase letters, then
    // YYMMDD (6 digits), then C or P, then 8-digit strike (price × 1000).
    static const QRegularExpression re(QStringLiteral("^[A-Z]{1,6}[0-9]{6}[CP][0-9]{8}$"));
    return re.match(symbol).hasMatch();
}

int option_contract_multiplier(const QString& symbol) {
    return is_occ_option_symbol(symbol) ? 100 : 1;
}

} // namespace openmarketterminal::trading
```

- [ ] **Step 7: Build + run — expect PASS.** Neuter-proof: change the regex `[CP]` → `[C]` (so puts stop matching), rebuild, confirm `occSymbolsAreOptions` FAILS on the SPY put; restore. Then make `option_contract_multiplier` always return 100; confirm `nonOptionsAreOne` FAILS; restore.

- [ ] **Step 8: Commit**
```bash
git add src/trading/options/OptionSymbol.h src/trading/options/OptionSymbol.cpp tests/tst_option_symbol.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(options): pure option_contract_multiplier (OCC detection → 100×)"
```

---

## Task 2: Apply the multiplier in both risk floors + validator OPRA

**Files:** Modify `src/mcp/tools/FastLiveTools.cpp` (~233), `src/mcp/tools/OrderFlowTools.cpp` (~112), `src/trading/OrderValidator.cpp` (~6); Modify `tests/tst_fast_live.cpp`.

- [ ] **Step 1: Write the failing safety-bite test** — add to `tests/tst_fast_live.cpp`. Use the existing arm-the-constitution helper + FakeBroker. Add a slot that: arms fast-live, sets a tight cap, and submits an OCC-symbol limit order whose `qty × price` is UNDER the cap but `×100` is OVER it → expect rejection; and the same notional as a plain equity symbol → accepted.
```cpp
// slot decl:
void fast_submit_option_uses_100x_multiplier();
```
```cpp
void TestFastLive::fast_submit_option_uses_100x_multiplier() {
    arm_fast_live();  // <-- use whatever the file's existing arm helper is called
    // tight cap: 50,000. An option at qty 1 × price 1000 = 1,000 raw, ×100 = 100,000 (OVER).
    SettingsRepository::instance().set("cli.risk.max_order_value", "50000");
    auto& prov = mcp::McpProvider::instance();
    // OCC option symbol, limit 1000 → 1 × 1000 × 100 = 100,000 > 50,000 → REJECTED
    auto opt = prov.call_tool("fast_submit_order", QJsonObject{
        {"symbol","AAPL260821C00110000"},{"side","buy"},{"quantity",1},
        {"order_type","limit"},{"limit_price",1000}});
    QVERIFY(opt.is_ok());
    QCOMPARE(opt.value().data.value("status").toString(), QString("rejected"));
    QVERIFY2(opt.value().data.value("reason").toString().contains("max order value"),
             qPrintable(opt.value().data.value("reason").toString()));
    // Same notional as a plain EQUITY symbol: 1 × 1000 × 1 = 1,000 < 50,000 → reaches broker (not this rejection)
    auto eq = prov.call_tool("fast_submit_order", QJsonObject{
        {"symbol","AAPL"},{"side","buy"},{"quantity",1},
        {"order_type","limit"},{"limit_price",1000},{"exchange","NASDAQ"}});
    QVERIFY(eq.is_ok());
    QVERIFY2(!eq.value().data.value("reason").toString().contains("max order value"),
             "equity at same raw notional must NOT hit the order-value cap");
}
```
(Adjust the `call_tool` result-access to match how other `tst_fast_live` tests read a ToolResult — grep an existing `fast_submit` test in the file and mirror its exact `.value().data` / `.value().result` accessor + the arm-helper name. Also `#include "storage/repositories/SettingsRepository.h"` if not present.)

- [ ] **Step 2: Build + run — expect FAIL** (multiplier not applied yet; the option order_value is 1,000 not 100,000, so it is NOT rejected):
```
cmake --build /tmp/ot-build-test --target tst_fast_live -j8 && ctest --test-dir /tmp/ot-build-test -R tst_fast_live --output-on-failure
```

- [ ] **Step 3: Apply the multiplier — fast floor.** In `src/mcp/tools/FastLiveTools.cpp`, add `#include "trading/options/OptionSymbol.h"` and change line ~233:
```cpp
    rv.order_value = o.quantity * resolved_price * trading::option_contract_multiplier(o.symbol);
```
(Leave `rv.max_loss = rv.order_value;` as-is — it now carries the ×100 exposure into the daily-loss check.)

- [ ] **Step 4: Apply the multiplier — equity floor.** In `src/mcp/tools/OrderFlowTools.cpp`, add the same include and change line ~112:
```cpp
    rv.order_value = o.quantity * resolved_price * trading::option_contract_multiplier(o.symbol);
```

- [ ] **Step 5: Validator OPRA.** In `src/trading/OrderValidator.cpp`, add `"OPRA"` to the `kExchanges` set (the `// US / global` group, ~line 11).

- [ ] **Step 6: Build + run — expect PASS** (`tst_fast_live` incl. the new slot). Neuter-proof: remove the `* trading::option_contract_multiplier(o.symbol)` from the fast floor, rebuild, confirm `fast_submit_option_uses_100x_multiplier` FAILS (option no longer rejected); restore. Full regression:
```
ctest --test-dir /tmp/ot-build-test -R "tst_fast_live|tst_order_flow|tst_option_symbol|tst_live_trading" --output-on-failure
```
Confirm the live build links: `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli -j8 2>&1 | tail -3`.

- [ ] **Step 7: Commit**
```bash
git add src/mcp/tools/FastLiveTools.cpp src/mcp/tools/OrderFlowTools.cpp src/trading/OrderValidator.cpp tests/tst_fast_live.cpp
git commit -m "fix(options): size risk floors ×100 for option contracts + validator accepts OPRA"
```

---

## Task 3: `AlpacaBroker::get_option_contracts` (+ IBroker default)

**Files:** Modify `src/trading/BrokerInterface.h`, `src/trading/brokers/alpaca/AlpacaBroker.{h,cpp}`. Build-verified (the live HTTP call is integration-verified in Task 4's manual smoke).

- [ ] **Step 1: Add the IBroker virtual with a safe default.** In `src/trading/BrokerInterface.h`, in the `IBroker` interface, add (near the other `ApiResponse<...> get_*` virtuals; ensure `<QJsonArray>` is available — add `#include <QJsonArray>` if needed):
```cpp
    /// Discover option contracts for an underlying. Default: not supported.
    /// params: underlying (required), type, expiry_gte, expiry_lte, strike_gte, strike_lte, limit.
    virtual ApiResponse<QJsonArray> get_option_contracts(const BrokerCredentials& /*creds*/,
                                                         const QJsonObject& /*params*/) {
        return {false, std::nullopt, "options discovery not supported for this broker", 0};
    }
```
(Match the exact `ApiResponse` brace-init shape the other defaults/returns use in this file — check a sibling like `get_quotes`'s return; the 4 fields are `{success, data, error, timestamp}`. Use `now_ts()` if a timestamp helper is in scope, else `0`.)

- [ ] **Step 2: Declare the override** in `src/trading/brokers/alpaca/AlpacaBroker.h` (next to `get_orders`):
```cpp
    ApiResponse<QJsonArray> get_option_contracts(const BrokerCredentials& creds,
                                                 const QJsonObject& params) override;
```

- [ ] **Step 3: Implement** in `src/trading/brokers/alpaca/AlpacaBroker.cpp` (mirror `get_orders`'s GET + parse pattern). Build the query from params, GET `/v2/options/contracts`, return the `option_contracts` array mapped to a compact shape:
```cpp
ApiResponse<QJsonArray> AlpacaBroker::get_option_contracts(const BrokerCredentials& creds,
                                                           const QJsonObject& params) {
    int64_t ts = now_ts();
    const QString underlying = params.value("underlying").toString().trimmed().toUpper();
    if (underlying.isEmpty())
        return {false, std::nullopt, "underlying is required", ts};

    QStringList q;
    q << "underlying_symbols=" + underlying;
    q << "status=active";
    if (params.contains("type")) {
        const QString t = params.value("type").toString().toLower();
        if (t == "call" || t == "put") q << "type=" + t;
    }
    if (params.contains("expiry_gte")) q << "expiration_date_gte=" + params.value("expiry_gte").toString();
    if (params.contains("expiry_lte")) q << "expiration_date_lte=" + params.value("expiry_lte").toString();
    if (params.contains("strike_gte")) q << "strike_price_gte=" + QString::number(params.value("strike_gte").toDouble());
    if (params.contains("strike_lte")) q << "strike_price_lte=" + QString::number(params.value("strike_lte").toDouble());
    int limit = params.contains("limit") ? params.value("limit").toInt() : 50;
    limit = qBound(1, limit, 200);
    q << "limit=" + QString::number(limit);

    auto resp = BrokerHttp::instance().get(trading_url(creds) + "/v2/options/contracts?" + q.join('&'),
                                           auth_headers(creds));
    if (!resp.success)
        return {false, std::nullopt, resp.error, ts};

    QJsonArray out;
    const auto doc = QJsonDocument::fromJson(resp.raw_body.toUtf8());
    for (const auto& v : doc.object().value("option_contracts").toArray()) {
        const auto o = v.toObject();
        out.append(QJsonObject{
            {"symbol", o.value("symbol").toString()},
            {"underlying", o.value("underlying_symbol").toString()},
            {"expiration", o.value("expiration_date").toString()},
            {"type", o.value("type").toString()},
            {"strike", o.value("strike_price").toVariant().toDouble()},
            {"close_price", o.value("close_price").toVariant().toDouble()},
            {"tradable", o.value("tradable").toBool()},
        });
    }
    return {true, out, "", ts};
}
```
(Add `#include <QJsonArray>`/`<QStringList>` if not present. Confirm `trading_url`/`auth_headers`/`now_ts` are the same helpers `get_orders` uses.)

- [ ] **Step 4: Build the GUI/CLI — must link** (proves the interface + override compile across all brokers):
```
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli -j8 2>&1 | tail -3
cmake --build /tmp/ot-build-test -j8 2>&1 | tail -3
```

- [ ] **Step 5: Commit**
```bash
git add src/trading/BrokerInterface.h src/trading/brokers/alpaca/AlpacaBroker.h src/trading/brokers/alpaca/AlpacaBroker.cpp
git commit -m "feat(options): AlpacaBroker.get_option_contracts (/v2/options/contracts)"
```

---

## Task 4: `get_option_contracts` MCP tool + registration

**Files:** Create `src/mcp/tools/OptionsTools.{cpp,h}`; Modify `src/mcp/McpInit.cpp`, `CMakeLists.txt`. Build-verified + integration-verified live.

- [ ] **Step 1: Study a read tool** — read `src/mcp/tools/MarketsTools.cpp` `get_quote` (ToolDef shape: `t.name`, `t.description`, `t.category`, `t.is_destructive=false`, `t.auth_required`, `t.input_schema`, `t.handler`) and how a fast-live read resolves the allowed account (`mcp::cli_allowed_account()` + `trading::AccountManager::instance().broker_for(acct)` + `load_credentials(acct)`).

- [ ] **Step 2: Create** `src/mcp/tools/OptionsTools.h`:
```cpp
#pragma once
#include "mcp/McpTypes.h"
#include <vector>
namespace openmarketterminal::mcp::tools {
std::vector<ToolDef> get_options_tools();
}
```

- [ ] **Step 3: Create** `src/mcp/tools/OptionsTools.cpp` — one **read-only, non-destructive** tool `get_option_contracts`:
```cpp
#include "mcp/tools/OptionsTools.h"
#include "mcp/tools/SettingsGate.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "mcp/ToolSchemaBuilder.h"   // match the builder other tools use; else build input_schema inline like MarketsTools
#include <QJsonObject>

namespace openmarketterminal::mcp::tools {

std::vector<ToolDef> get_options_tools() {
    ToolDef t;
    t.name = "get_option_contracts";
    t.description = "List tradable option contracts for an underlying (Alpaca). "
                    "Returns OCC symbols + strike/expiry/type/last price to pick a contract to trade.";
    t.category = "markets";
    t.is_destructive = false;
    // build input_schema: required string 'underlying'; optional 'type'(call|put),
    // 'expiry_gte','expiry_lte' (YYYY-MM-DD), 'strike_gte','strike_lte' (number),
    // 'limit' (number). Mirror MarketsTools::get_quote's schema construction exactly.
    t.handler = [](const QJsonObject& args) -> ToolResult {
        const QString underlying = args.value("underlying").toString().trimmed();
        if (underlying.isEmpty())
            return ToolResult::fail("Missing 'underlying'");
        const QString acct = mcp::cli_allowed_account();
        if (acct.isEmpty() || !trading::AccountManager::instance().has_account(acct))
            return ToolResult::fail("no allowed account configured for option discovery");
        trading::IBroker* broker = trading::AccountManager::instance().broker_for(acct);
        if (!broker)
            return ToolResult::fail("broker unavailable for the allowed account");
        const auto creds = trading::AccountManager::instance().load_credentials(acct);
        const auto resp = broker->get_option_contracts(creds, args);
        if (!resp.success || !resp.data.has_value())
            return ToolResult::fail(resp.error.isEmpty() ? "option discovery failed" : resp.error);
        return ToolResult::ok_data(QJsonObject{{"contracts", resp.data.value()},
                                               {"count", resp.data.value().size()}});
    };
    return {t};
}

} // namespace openmarketterminal::mcp::tools
```
(VERIFY against the real `ToolDef`/`ToolResult`/`ToolSchemaBuilder` API — `ToolResult::ok_data`/`fail`, schema builder method names, `cli_allowed_account` signature. Mirror `MarketsTools::get_quote` for the schema + result idioms exactly. `AccountManager::has_account`/`broker_for`/`load_credentials` exist per AccountManager.h.)

- [ ] **Step 4: Register** in `src/mcp/McpInit.cpp` `register_core_tools()`: add `#include "mcp/tools/OptionsTools.h"` and `provider.register_tools(tools::get_options_tools());` near the other read-tool registrations (e.g. after `get_markets_tools()`).

- [ ] **Step 5: Add to CMake core sources** — add `src/mcp/tools/OptionsTools.cpp` to the `openterminal_core` source list in `CMakeLists.txt` (near the other `src/mcp/tools/*.cpp`).

- [ ] **Step 6: Build the GUI/CLI — must link + the tool registers:**
```
cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli -j8 2>&1 | tail -3
```

- [ ] **Step 7: Integration smoke (manual, with the user — Alpaca live):** with the GUI running + the Alpaca paper account as `cli.allowed_account`:
```
openterminalcli mcp call get_option_contracts '{"underlying":"AAPL","type":"call","expiry_gte":"2026-08-01","limit":5}'
```
Expect a list of contracts (OCC symbol + strike + expiry + close_price). Then a full options round-trip: pick a returned `symbol`, `fast_submit_order` a 1-contract limit on it → accepted as `us_option` (risk floor now ×100), then `cancel_order`.

- [ ] **Step 8: Commit**
```bash
git add src/mcp/tools/OptionsTools.h src/mcp/tools/OptionsTools.cpp src/mcp/McpInit.cpp CMakeLists.txt
git commit -m "feat(options): get_option_contracts MCP tool (discovery via allowed account)"
```

---

## Final review (after all tasks)
Dispatch a reviewer: (a) `option_contract_multiplier` matches OCC exactly + no equity/crypto false-positive; (b) BOTH risk floors apply the multiplier (and `max_loss` carries it into daily-loss); (c) `get_option_contracts` is read-only, account-scoped, error-tolerant (no allowed account / non-Alpaca / HTTP error → clean failure, no crash); (d) no regression (`ctest -R "tst_option_symbol|tst_fast_live|tst_order_flow|tst_live_trading|tst_settings_gate"`). Then `superpowers:finishing-a-development-branch`.

---

## Self-review (against the spec)
- **Spec coverage:** safety multiplier (T1 pure + T2 applied in both floors), validator OPRA (T2), AlpacaBroker.get_option_contracts (T3), discovery MCP tool routed to allowed account (T4), order path unchanged (no task — already works). ✓
- **Placeholder scan:** every code step has concrete code; the "mirror MarketsTools / verify the ToolDef API" notes in T4 are deliberate pattern-matching against real headers, not placeholders. No TBD/TODO. ✓
- **Type consistency:** `is_occ_option_symbol`/`option_contract_multiplier` (T1) used verbatim in T2; `ApiResponse<QJsonArray> get_option_contracts(creds, params)` identical in T3 (interface + override) and T4 (caller); tool name `get_option_contracts`, params `underlying/type/expiry_gte/expiry_lte/strike_gte/strike_lte/limit` consistent across T3/T4/spec. ✓
- **Safety bite is load-bearing:** T2's test rejects an option over the ×100 cap that would pass at ×1, neuter-proofed. ✓
