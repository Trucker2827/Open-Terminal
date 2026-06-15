# IBKR conid Resolution — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let the IBKR adapter fill orders / fetch quotes / fetch history from a **plain ticker** (e.g. `AAPL`) by resolving the symbol to an IBKR `conid` via the Client Portal `/iserver/secdef/search` endpoint, so `submit_order` / `fast_submit_order` work on IBKR without the caller knowing the numeric contract id.

**Architecture:** The substrate sends a plain symbol with an empty `instrument_token`. Today `IBKRBroker::place_order`, `get_quotes`, and `get_history` all hard-fail with "conid required (format EXCHANGE:SYMBOL:CONID)". Add one reusable private helper `resolve_conid(symbol, gw, headers, *err)` that GETs `/iserver/secdef/search?symbol=<sym>` and a **pure, unit-testable** static parser `parse_conid_from_secdef(body, symbol)` that extracts the stock (STK) contract's conid. Wire resolution into all three conid sites: when the conid can't be extracted from `instrument_token` or the `EXCH:SYM:CONID` symbol form, resolve it from the plain symbol. Add a small thread-safe static `symbol→conid` cache (conids are stable; avoids re-resolving in a strategy loop). Existing `EXCH:SYM:CONID` and `instrument_token` paths are preserved (no resolution round-trip when the conid is already known).

**Tech Stack:** Qt6 C++20, `BrokerHttp::instance()` (singleton; not mockable → test the pure parser), QtTest.

**Constraints:** Sandbox/no-real-creds. Surgical — do not change the substrate's plain-ticker intent, the constitution, or other adapters. The HTTP round-trip is integration-tested by the user against a live gateway later; this task unit-tests the parser (the logic that can be wrong) with fixtures.

---

### Task 1: Symbol→conid resolution in the IBKR adapter

**Files:**
- Modify: `src/trading/brokers/ibkr/IBKRBroker.h` (declare the two helpers)
- Modify: `src/trading/brokers/ibkr/IBKRBroker.cpp` (implement helpers; wire into `place_order` ~191-199, `get_quotes` ~497-523, `get_history` ~570-576)
- Create: `tests/tst_ibkr_conid.cpp`
- Modify: `tests/CMakeLists.txt` (register `tst_ibkr_conid`)

#### Header declarations

In `IBKRBroker.h`, add to the **public** static section (next to `is_token_expired` / `checked_error`):

```cpp
    /// Parse the conid for a stock (STK) contract matching `symbol` from a
    /// /iserver/secdef/search response body. Returns "" if no match / malformed.
    /// Pure (no I/O) — unit-tested.
    static QString parse_conid_from_secdef(const QByteArray& body, const QString& symbol);
```

In the **private** section (next to `gateway_url`):

```cpp
    /// Resolve a plain symbol to an IBKR conid via /iserver/secdef/search.
    /// Caches results (conids are stable). Returns "" and sets *err on failure.
    static QString resolve_conid(const QString& symbol, const QString& gw,
                                 const QMap<QString, QString>& headers, QString* err);
```

#### Parser semantics (`parse_conid_from_secdef`)

The `/iserver/secdef/search?symbol=AAPL` response is a JSON **array**; each element looks like:
```json
{ "conid": "265598", "symbol": "AAPL", "companyName": "APPLE INC",
  "description": "NASDAQ", "sections": [ {"secType":"STK"}, {"secType":"OPT"} ] }
```
Selection rule:
1. Parse the array. For each element whose `symbol` equals the requested symbol (case-insensitive): if any entry in `sections[]` has `secType == "STK"`, return that element's `conid` (as a string; handle both string and numeric JSON).
2. Fallback: the first element whose `symbol` matches the request (regardless of sections) → its `conid`.
3. No symbol match / not an array / empty / malformed → return `""`.

#### Resolver (`resolve_conid`)

```cpp
QString IBKRBroker::resolve_conid(const QString& symbol, const QString& gw,
                                  const QMap<QString, QString>& headers, QString* err) {
    static QMutex cache_mtx;
    static QMap<QString, QString> cache;  // upper(symbol) -> conid
    const QString key = symbol.toUpper();
    {
        QMutexLocker lock(&cache_mtx);
        auto it = cache.find(key);
        if (it != cache.end()) return it.value();
    }
    auto& http = BrokerHttp::instance();
    auto resp = http.get(gw + "/v1/api/iserver/secdef/search?symbol=" + symbol, headers);
    if (!resp.success) {
        if (err) *err = checked_error(resp, "secdef/search failed");
        return {};
    }
    QString conid = parse_conid_from_secdef(resp.raw_body.toUtf8(), symbol);
    if (!conid.isEmpty()) {
        QMutexLocker lock(&cache_mtx);
        cache.insert(key, conid);
    }
    return conid;
}
```

#### Wiring — `place_order` (replace the conid block ~191-199)

```cpp
    // conid: prefer instrument_token, then the "EXCH:SYMBOL:CONID" form,
    // then resolve a plain ticker via secdef/search.
    QString conid = order.instrument_token;
    if (conid.isEmpty()) {
        QStringList parts = order.symbol.split(":");
        if (parts.size() >= 3)
            conid = parts[2];
    }
    if (conid.isEmpty() && !order.symbol.isEmpty()) {
        // plain ticker (e.g. "AAPL") or "EXCH:SYM" — resolve the contract
        const QString sym = order.symbol.contains(":") ? order.symbol.section(':', -1) : order.symbol;
        QString err;
        conid = resolve_conid(sym, gw, auth_headers(creds), &err);
        if (conid.isEmpty())
            return {false, "", "IBKR place_order: could not resolve conid for " + sym +
                                   (err.isEmpty() ? "" : " — " + err)};
    }
    if (conid.isEmpty())
        return {false, "", "IBKR place_order: conid required (symbol format: EXCHANGE:SYMBOL:CONID)"};
```

#### Wiring — `get_quotes` (the per-symbol loop ~509-520)

For each requested symbol: if it parses to `EXCH:SYM:CONID` use parts[2]; else resolve the plain symbol via `resolve_conid(...)`. Keep the existing `conid_to_name` reverse map (map the resolved conid back to the **plain** symbol the caller passed so the returned `quote.symbol` is what they asked for). If resolution fails for a symbol, skip it (do not abort the whole batch). Keep the existing "no conids at all" error.

#### Wiring — `get_history` (~570-576)

If the symbol parses to `EXCH:SYM:CONID` use parts[2]; else `resolve_conid(plain_symbol, ...)`. On failure keep the existing "conid required" style error (now: "could not resolve conid for <sym>").

#### Test file `tests/tst_ibkr_conid.cpp` (QtTest)

Unit-test the pure parser with fixtures (no gateway, no network):
- **STK match:** an AAPL response (array, element has `symbol:"AAPL"`, `sections` with `{"secType":"STK"}`, `conid:"265598"`) → returns `"265598"`.
- **Numeric conid:** same but `"conid": 265598` (JSON number) → returns `"265598"`.
- **Picks STK among multiple:** array with a non-STK element first (e.g. an index/FUT entry, or a different `symbol`) and the STK element second → returns the STK element's conid.
- **Case-insensitive request:** request `"aapl"` against `symbol:"AAPL"` → returns the conid.
- **No match:** array of elements none matching the symbol → returns `""`.
- **Malformed / not array / empty body:** `"{}"`, `""`, `"garbage"` → returns `""` (no crash).

Register in `tests/CMakeLists.txt` mirroring `tst_fast_live` (lines 147-150):
```cmake
add_executable(tst_ibkr_conid tst_ibkr_conid.cpp)
target_include_directories(tst_ibkr_conid PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ibkr_conid PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ibkr_conid COMMAND tst_ibkr_conid)
```

#### Steps

- [ ] Write `tests/tst_ibkr_conid.cpp` with the failing cases above; register in CMake.
- [ ] Configure/build the test target; run `ctest -R tst_ibkr_conid` → FAIL (parser undefined / wrong).
- [ ] Implement `parse_conid_from_secdef` + `resolve_conid` in the header/cpp.
- [ ] Wire resolution into `place_order`, `get_quotes`, `get_history`.
- [ ] Build; run `ctest -R tst_ibkr_conid` → PASS. **Neuter-proof:** break the STK-selection line, confirm a test fails, restore.
- [ ] Run the full trading test set (`ctest -R "tst_fast_live|tst_order_flow|tst_live_trading|tst_settings_gate|tst_strategy_loop"`) → all PASS (no regressions; the IBKR adapter is in `openterminal_core`).
- [ ] Commit on `feat/ibkr-conid`.

**Out of scope (note, don't build):** live-gateway integration test (user runs against their gateway); options/futures contract disambiguation beyond STK; per-exchange listing preference; modify/cancel (use `order_id`, no conid needed).
