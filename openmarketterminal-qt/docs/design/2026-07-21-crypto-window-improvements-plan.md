# Crypto Window Improvements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the crypto trading screen Coinbase-native-correct, give it real-time account data + a live ladder overlay, honest freshness-gated paper fills, and local alerts — per `docs/design/2026-07-21-crypto-window-improvements-design.md`.

**Architecture:** Pure, unit-tested cores (symbol universe, chrome state, fill model, alert engine, order normalizer) added as small headers; GUI files consume them with minimal surgical edits. Account streaming extends the existing `ws_stream.py` ccxt.pro subprocess + DataHub topic pattern — no new C++ network clients.

**Tech Stack:** Qt6/C++17 (QtTest via ctest), Python 3.11 (ccxt/ccxt.pro, unittest), CMake+Ninja.

## Global Constraints

- **DUEL SAFETY (from spec, binding):** The Claude-vs-Codex duel is LIVE. Branch/build/test freely, but **NEVER install a new app binary, restart the serve daemon, or touch launchd jobs**. Merged ≠ deployed. Do NOT modify `cli/ServeCommand.cpp`, `scripts/kalshi_advise/*`, or advisor state dirs. The single sanctioned exception is Task 5's confined edit in `KalshiScreen.cpp` (fallback-guarded).
- **Build (tests):** `cmake -S openmarketterminal-qt -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENMARKETTERMINAL_BUILD_TESTS=ON` then `cmake --build /tmp/ot-build-test --target <tst_target>`; run via `ctest --test-dir /tmp/ot-build-test -R <name> -V`.
- **Python tests:** run from `openmarketterminal-qt/`: `python3 -m unittest tests.test_ws_stream_account -v`.
- **Verification discipline:** every behavioral test must be **neuter-verified** (temporarily break the code, confirm the test FAILS, restore). QtTest `QVERIFY/QCOMPARE` only — no bare `assert()`.
- **Surgical edits only** — match surrounding style; do not reformat untouched code.
- One branch per phase (`feat/crypto-p1-coinbase-correctness`, `feat/crypto-p2-account-ws`, `feat/crypto-p3-honest-paper`, `feat/crypto-p4-alerts`); commit per task.
- Namespaces: pure crypto-screen helpers go in `openmarketterminal::crypto` (like `CryptoLadderModel.h`); trading-layer types in `openmarketterminal::trading`.

**Plan-vs-spec deviations (approved rationale, surfaced for review):**
1. **P1-2 scope shrank:** `set_live_auth_indicator(bool)` already exists (`CryptoTradingScreen_Refresh.cpp:166`) and colors `api_btn_` + `ws_transport_`. Remaining gaps only: (a) a third "no credentials" neutral state, (b) DAEMON label should reflect *daemon liveness*, not mirror auth.
2. **P2-2 is smaller than spec feared:** `CryptoLadderModel` ALREADY supports `MyOrder` overlays + avg-entry (integer-bucket keyed). The work is normalizing live-order JSON → `MyOrder` and VWAP-ing my-trades, then calling the existing `ladder_->set_my_orders()/set_avg_entry()`.
3. **P3 does NOT route through `services/sandbox/PaperExecutor`:** audit shows `PaperExecutor::run_cycle` is journal-strategy-driven (scans `edge_decision_journal`/`scalp_decisions.jsonl`), not a manual-order engine — forcing manual GUI orders through it would be a misfit rebuild. The spec's *intent* (walk-the-book fills, fee-honest, stale-book REJECTION) is implemented as a pure `CryptoPaperFill` core wired into the existing `pt_place_order`/`pt_fill_order` flow. Consequence: NO `PtPortfolio` migration/bankroll reset is needed (spec's migration section is moot). The current dishonest path being replaced: paper market orders fill at `ticker.last` **or a literal `1000.0` fallback** with no staleness check (`CryptoTradingScreen_Handlers.cpp:371-392`).
4. Limit-order paper fills stay with `OrderMatcher` (touch-fill maker model — acceptable); P3 scopes to **market-order** honesty + stale rejection.

---

# Phase 1 — Coinbase-correctness (branch `feat/crypto-p1-coinbase-correctness`)

### Task 1: `CryptoSymbolUniverse` pure helper

**Files:**
- Create: `src/screens/crypto_trading/CryptoSymbolUniverse.h`
- Create: `tests/tst_crypto_symbol_universe.cpp`
- Modify: `tests/CMakeLists.txt` (append registration)

**Interfaces:**
- Produces: `namespace openmarketterminal::crypto` — `QString quote_currency_for(const QString& exchange_id)`, `QStringList default_watchlist_for(const QString& exchange_id, bool bitcoin_focus)`, `QString default_symbol_for(const QString& exchange_id, bool bitcoin_focus)`, `QString migrate_symbol(const QString& exchange_id, const QString& symbol)`. Task 2 consumes all four.

- [ ] **Step 1: Write the failing test** — `tests/tst_crypto_symbol_universe.cpp`:

```cpp
#include "screens/crypto_trading/CryptoSymbolUniverse.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

class TstCryptoSymbolUniverse : public QObject {
    Q_OBJECT
  private slots:
    void quote_currency() {
        QCOMPARE(quote_currency_for("coinbase"), QString("USD"));
        QCOMPARE(quote_currency_for("kraken"), QString("USD"));
        QCOMPARE(quote_currency_for("binance"), QString("USDT"));
        QCOMPARE(quote_currency_for("okx"), QString("USDT"));
        QCOMPARE(quote_currency_for("unknown_exchange"), QString("USDT")); // conservative default
    }
    void default_symbol() {
        // Bitcoin-focus is venue-consistent: BTC quoted in the venue's native
        // currency (the old hardcoded BTC/USD-under-focus was Coinbase-implicit).
        QCOMPARE(default_symbol_for("coinbase", false), QString("BTC/USD"));
        QCOMPARE(default_symbol_for("binance", false), QString("BTC/USDT"));
        QCOMPARE(default_symbol_for("coinbase", true), QString("BTC/USD"));
        QCOMPARE(default_symbol_for("binance", true), QString("BTC/USDT"));
    }
    void watchlist_no_usdt_on_coinbase() {
        const QStringList wl = default_watchlist_for("coinbase", false);
        QVERIFY(wl.size() >= 20);
        for (const QString& s : wl) QVERIFY2(!s.endsWith("/USDT"), qPrintable(s));
        QVERIFY(wl.contains("BTC/USD"));
    }
    void watchlist_binance_keeps_usdt() {
        const QStringList wl = default_watchlist_for("binance", false);
        QVERIFY(wl.contains("BTC/USDT"));
    }
    void migrate_quote_suffix_only() {
        QCOMPARE(migrate_symbol("coinbase", "ETH/USDT"), QString("ETH/USD"));  // remap quote
        QCOMPARE(migrate_symbol("coinbase", "ETH/USD"),  QString("ETH/USD"));  // already right
        QCOMPARE(migrate_symbol("binance",  "ETH/USDT"), QString("ETH/USDT")); // no-op
        QCOMPARE(migrate_symbol("coinbase", "WEIRD"),    QString("WEIRD"));    // unknown shape untouched
        QCOMPARE(migrate_symbol("coinbase", "BTC/USDC"), QString("BTC/USDC")); // user's explicit USDC pair untouched
    }
};
QTEST_MAIN(TstCryptoSymbolUniverse)
#include "tst_crypto_symbol_universe.moc"
```

- [ ] **Step 2: Register + run to verify it fails** — append to `tests/CMakeLists.txt` (mirror the `tst_crypto_ladder_model` block at lines 354-357):

```cmake
add_executable(tst_crypto_symbol_universe tst_crypto_symbol_universe.cpp)
target_include_directories(tst_crypto_symbol_universe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_symbol_universe PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_symbol_universe COMMAND tst_crypto_symbol_universe)
```

Run: `cmake --build /tmp/ot-build-test --target tst_crypto_symbol_universe` → Expected: **compile FAIL** (header does not exist).

- [ ] **Step 3: Implement** — `src/screens/crypto_trading/CryptoSymbolUniverse.h` (header-only, pure, mirrors `CryptoLadderModel.h` style):

```cpp
#pragma once
// Per-exchange symbol universe — the display pair IS the wire pair.
// Kills the Coinbase "/USDT displayed, /USD on the wire" remap fiction.
// Pure: no Qt widgets, no I/O.

#include <QString>
#include <QStringList>

namespace openmarketterminal::crypto {

/// Native quote currency for an exchange's default spot universe.
/// USD-quoted venues list explicitly; everything else (binance, okx, bybit,
/// kucoin, bitget, gate, mexc, unknown) conservatively keeps USDT.
inline QString quote_currency_for(const QString& exchange_id) {
    if (exchange_id == QLatin1String("coinbase") || exchange_id == QLatin1String("kraken"))
        return QStringLiteral("USD");
    return QStringLiteral("USDT");
}

/// Default watchlist bases, quoted in the exchange's native currency.
inline QStringList default_watchlist_for(const QString& exchange_id, bool bitcoin_focus) {
    const QString q = quote_currency_for(exchange_id);
    if (bitcoin_focus)
        return {QStringLiteral("BTC/") + q};
    static const QStringList kBases = {
        "BTC", "ETH", "SOL", "BNB", "XRP",  "DOGE", "ADA", "AVAX", "TON",
        "LINK", "DOT", "MATIC", "UNI", "ATOM", "LTC", "BCH", "APT", "ARB",
        "OP",  "SUI", "TRX", "INJ", "NEAR", "WIF", "PEPE",
    };
    QStringList out;
    out.reserve(kBases.size());
    for (const QString& b : kBases)
        out << b + QLatin1Char('/') + q;
    return out;
}

inline QString default_symbol_for(const QString& exchange_id, bool /*bitcoin_focus*/) {
    return QStringLiteral("BTC/") + quote_currency_for(exchange_id);
}

/// Persisted-state migration: remap ONLY a stale default-quote suffix to the
/// exchange's native quote. A user's explicit non-default pair (e.g.
/// BTC/USDC) and unknown shapes pass through untouched.
inline QString migrate_symbol(const QString& exchange_id, const QString& symbol) {
    const QString native = quote_currency_for(exchange_id);
    const QString stale  = (native == QLatin1String("USD")) ? QStringLiteral("USDT") : QString();
    if (stale.isEmpty())
        return symbol;
    const QString suffix = QLatin1Char('/') + stale;
    if (!symbol.endsWith(suffix))
        return symbol;
    return symbol.left(symbol.size() - stale.size()) + native;
}

} // namespace openmarketterminal::crypto
```

- [ ] **Step 4: Run to verify pass** — `ctest --test-dir /tmp/ot-build-test -R tst_crypto_symbol_universe -V` → Expected: PASS. **Neuter:** change `"USD"` → `"USDT"` for coinbase in `quote_currency_for`, rebuild, confirm FAIL, restore.

- [ ] **Step 5: Commit**

```bash
git add src/screens/crypto_trading/CryptoSymbolUniverse.h tests/tst_crypto_symbol_universe.cpp tests/CMakeLists.txt
git commit -m "feat(crypto): per-exchange symbol universe helper (Coinbase=/USD)"
```

### Task 2: Wire the symbol universe into the screen

**Files:**
- Modify: `src/screens/crypto_trading/CryptoTradingScreen.h:162-196` (drop the hardcoded initializers)
- Modify: `src/screens/crypto_trading/CryptoTradingScreen.cpp:62-74` (ctor), `:377-390` (`default_symbol`), `:728` (`restore_state`)
- Modify: `src/screens/crypto_trading/CryptoTradingScreen_Handlers.cpp` (`on_exchange_changed`, `order_symbol_for_exchange:100`)

**Interfaces:**
- Consumes: Task 1's four functions.
- Produces: `watchlist_symbols_` is now always exchange-native; `order_symbol_for_exchange()` becomes identity for coinbase default pairs (assert, keep for funding-currency override).

- [ ] **Step 1:** In `CryptoTradingScreen.h`, replace the member initializers: `QString selected_symbol_ = "BTC/USDT";` → `QString selected_symbol_;` and delete the 25-entry `watchlist_symbols_` initializer list (lines 191-196) → `QStringList watchlist_symbols_;`.

- [ ] **Step 2:** In the ctor (`CryptoTradingScreen.cpp:62-74`), before `setup_ui()`:

```cpp
    selected_symbol_   = crypto::default_symbol_for(exchange_id_, bitcoin_focus_);
    watchlist_symbols_ = crypto::default_watchlist_for(exchange_id_, bitcoin_focus_);
```

(delete the old `if (bitcoin_focus_)` block that hardcoded these; add `#include "screens/crypto_trading/CryptoSymbolUniverse.h"`). Rewrite `default_symbol()` (`:377-378`) as `return crypto::default_symbol_for(exchange_id_, bitcoin_focus_);`.

- [ ] **Step 3:** In `on_exchange_changed` (Handlers), after `exchange_id_ = exchange;` and the buffer-clearing block, re-derive the universe and migrate the selection:

```cpp
    watchlist_symbols_ = crypto::default_watchlist_for(exchange_id_, bitcoin_focus_);
    if (watchlist_) watchlist_->set_symbols(watchlist_symbols_);
    selected_symbol_ = crypto::migrate_symbol(exchange_id_, selected_symbol_);
```

In `restore_state` (`CryptoTradingScreen.cpp:728`), wrap the restored symbol: `switch_symbol(crypto::migrate_symbol(exchange_id_, state.value("selected_symbol", default_symbol()).toString()));` — a persisted `ETH/USDT` on Coinbase comes back as `ETH/USD` and is persisted back on next save. User-added non-default pairs survive (only the stale default-quote suffix is remapped).

- [ ] **Step 4:** In `order_symbol_for_exchange` (`Handlers.cpp:100`): remove any coinbase `/USDT→/USD` remap branch it contains (read it first — keep the funding-currency override logic). Add a `LOG_WARN` if display≠execution for coinbase absent a funding override (should now never fire).

- [ ] **Step 5: Build + full crypto test sweep** — `cmake --build /tmp/ot-build-test && ctest --test-dir /tmp/ot-build-test -R "crypto" -V` → all PASS. Also grep-verify: `grep -rn '"BTC/USDT"' src/screens/crypto_trading/` → only historical comments remain (no code literals).

- [ ] **Step 6: Commit** — `git commit -am "feat(crypto): exchange-native watchlist/default symbol; migrate persisted USDT pairs"`

### Task 3: DAEMON-liveness + tri-state API chrome

**Files:**
- Create: `src/screens/crypto_trading/CryptoChromeState.h`
- Create: `tests/tst_crypto_chrome_state.cpp` (+ CMake registration, same 4-line pattern as Task 1)
- Modify: `src/screens/crypto_trading/CryptoTradingScreen_Refresh.cpp:166-184` (`set_live_auth_indicator`)

**Interfaces:**
- Produces: `crypto::chrome_api_state(bool has_credentials, int last_auth_state) -> QString` ("none"|"ok"|"error") and `crypto::chrome_daemon_state(bool daemon_alive, bool ws_connected) -> QString` ("dead"|"rest"|"live"). Screen maps these onto the existing dynamic-property + polish pattern.

- [ ] **Step 1: Failing test** — `tests/tst_crypto_chrome_state.cpp`:

```cpp
#include "screens/crypto_trading/CryptoChromeState.h"
#include <QtTest/QtTest>
using namespace openmarketterminal::crypto;

class TstCryptoChromeState : public QObject {
    Q_OBJECT
  private slots:
    void api_states() {
        QCOMPARE(chrome_api_state(false, -1), QString("none"));   // no creds → neutral
        QCOMPARE(chrome_api_state(false,  1), QString("none"));   // creds gone → neutral wins
        QCOMPARE(chrome_api_state(true,  -1), QString("none"));   // creds but never probed
        QCOMPARE(chrome_api_state(true,   1), QString("ok"));
        QCOMPARE(chrome_api_state(true,   0), QString("error"));
    }
    void daemon_states() {
        QCOMPARE(chrome_daemon_state(false, false), QString("dead"));
        QCOMPARE(chrome_daemon_state(false, true),  QString("dead")); // dead daemon wins over stale ws flag
        QCOMPARE(chrome_daemon_state(true,  false), QString("rest"));
        QCOMPARE(chrome_daemon_state(true,  true),  QString("live"));
    }
};
QTEST_MAIN(TstCryptoChromeState)
#include "tst_crypto_chrome_state.moc"
```

Run target build → compile FAIL (header missing).

- [ ] **Step 2: Implement** — `CryptoChromeState.h`:

```cpp
#pragma once
// Truthful status-chrome state mapping for the crypto command bar.
// Pure string mapping so the tri-state rules are unit-testable.
#include <QString>
namespace openmarketterminal::crypto {

/// API button: "none" (no credentials / never probed), "ok" (last
/// authenticated call succeeded), "error" (credentials present, last call failed).
inline QString chrome_api_state(bool has_credentials, int last_auth_state) {
    if (!has_credentials || last_auth_state < 0) return QStringLiteral("none");
    return last_auth_state == 1 ? QStringLiteral("ok") : QStringLiteral("error");
}

/// DAEMON label: "dead" (subprocess not running), "rest" (daemon up, public
/// WS not connected — REST fallback), "live" (daemon up + WS streaming).
inline QString chrome_daemon_state(bool daemon_alive, bool ws_connected) {
    if (!daemon_alive) return QStringLiteral("dead");
    return ws_connected ? QStringLiteral("live") : QStringLiteral("rest");
}

} // namespace openmarketterminal::crypto
```

- [ ] **Step 3: Run test** → PASS. Neuter (`return "ok"` unconditionally in `chrome_api_state`) → FAIL → restore.

- [ ] **Step 4: Wire the screen.** In `set_live_auth_indicator` (`Refresh.cpp:166`): replace the two-state property write on `api_btn_` with `api_btn_->setProperty("authed", crypto::chrome_api_state(!ExchangeService::instance().get_credentials().api_key.isEmpty(), last_auth_state_))` (check the actual `ExchangeCredentials` field name in `trading/TradingTypes.h` — use the key field that `CryptoCredentials.cpp` writes). **Stop applying the auth property to `ws_transport_`** (that conflation is the bug). Instead, in `flush_ws_updates` (`Refresh.cpp:186`, runs at 10fps but property writes are edge-detected) add a daemon-state hook mirroring `apply_feed_mode`: track `int last_daemon_chrome_ = -1;` (new header member), compute `chrome_daemon_state(ExchangeService::instance().wait_for_daemon_ready(0), ExchangeService::instance().is_ws_connected())` — if `wait_for_daemon_ready(0)` busy-blocks, expose the pool's existing non-blocking readiness instead (check `ExchangeDaemonPool` for an `is_ready()`; add a const passthrough on `ExchangeService` if absent — 3 lines, no behavior change). Map to text/tooltip: dead→"DAEMON ✕ (subprocess down)", rest→"DAEMON · REST", live→"DAEMON · WS". Update the QSS-relevant dynamic property + `unpolish/polish` exactly like the existing pattern at `Refresh.cpp:171-177`.

- [ ] **Step 5: Build + run crypto tests; commit** — `git commit -am "feat(crypto): tri-state API chrome + truthful DAEMON liveness label"`

### Task 4: Advanced Trade REST migration (ticker + candles in CommandDispatch)

**Files:**
- Create: `src/services/crypto/CoinbaseEndpoints.h`
- Create: `tests/tst_coinbase_endpoints.cpp` (+ CMake registration; link `openterminal_core` like `tst_crypto_risk` if the parse helpers land in core, else header-only + `Qt6::Core Qt6::Test`)
- Modify: `src/cli/CommandDispatch.cpp:20244` (ticker URL + parse), `:20533` (candles URL + parse)

**Interfaces:**
- Produces: `namespace openmarketterminal::services::crypto` — `QUrl advanced_ticker_url(const QString& product)`, `QUrl advanced_candles_url(const QString& product, qint64 start_s, qint64 end_s)`, `bool parse_advanced_ticker(const QJsonDocument&, double* price, double* bid, double* ask)`, `QVector<AdvCandle> parse_advanced_candles(const QJsonDocument&)` with `struct AdvCandle { qint64 open_ms; double open, high, low, close, volume; }`.
- Consumes: nothing new. `CommandDispatch.cpp` call sites swap URL + parse only; `EdgeCollectedTick`/`EdgeBackfillCandle` population logic stays.

**Endpoint facts (public, no auth):** ticker `GET https://api.coinbase.com/api/v3/brokerage/market/products/<PRODUCT>/ticker?limit=1` → `{"trades":[{"price":"...","time":...}],"best_bid":"...","best_ask":"..."}`; candles `GET .../market/products/<PRODUCT>/candles?start=<unix_s>&end=<unix_s>&granularity=ONE_MINUTE` → `{"candles":[{"start":"<unix_s>","low":"...","high":"...","open":"...","close":"...","volume":"..."}]}` (values are STRINGS; max 300 candles/request — same 300-page chunking as the legacy loop at `:20531`).

- [ ] **Step 1: Capture real fixtures** (development-only script, not committed to app code): `curl -s "https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD/ticker?limit=1" > tests/fixtures/coinbase_adv_ticker.json` and the candles equivalent (any 10-minute window) → `tests/fixtures/coinbase_adv_candles.json`. Commit fixtures. If the live capture returns an unexpected shape, STOP and re-verify the endpoint docs before writing the parser — the fixture IS the contract.

- [ ] **Step 2: Failing test** — load each fixture file, assert `parse_advanced_ticker` yields price/bid/ask > 0 and `parse_advanced_candles` yields ≥1 candle with `low ≤ high`, `open_ms` ms-scaled (`> 1e12`), all six fields > 0. Also assert `advanced_candles_url("BTC-USD", 1700000000, 1700000600).toString()` contains `granularity=ONE_MINUTE`, both unix-second params, and host `api.coinbase.com`. Build → FAIL (header missing).

- [ ] **Step 3: Implement `CoinbaseEndpoints.h`** — header-only; parsers read the string-typed numbers via `QJsonValue::toString().toDouble()` with a `toDouble()` fallback for forward compatibility; `open_ms = start_s * 1000`. Then edit the two `CommandDispatch.cpp` sites: `:20244` swap the QUrl to `advanced_ticker_url(out.venue_symbol)` and adapt the response-read to `parse_advanced_ticker` (the legacy exchange API returned `{"price","bid","ask"}` top-level; keep populating the same `EdgeCollectedTick` fields). `:20533` swap URL builder + replace the positional-array row parse (`row.at(0..5)`) with `parse_advanced_candles`, keeping the cursor/`pages < 3000` pagination and the `open_ms >= from_ms` window filter identical.

- [ ] **Step 4: Run tests** — endpoint test PASS; neuter (`open_ms = start_s` without ×1000) → FAIL → restore. Build the full app target compiles: `cmake --build /tmp/ot-build-test`.

- [ ] **Step 5: Grep gate + commit** — `grep -rn "api.exchange.coinbase.com" src/cli/` → zero hits. `git add -A && git commit -m "feat(crypto): migrate CommandDispatch Coinbase REST to Advanced Trade public API"`

### Task 5: Advanced Trade WS migration (latency service + Kalshi spot reference), fallback-guarded

**Files:**
- Modify: `src/services/crypto_latency/CryptoLatencyService.cpp:325` (+ its message-parse and subscribe-frame code — locate via `grep -n "level2\|subscribe" src/services/crypto_latency/CryptoLatencyService.cpp`)
- Modify: `src/screens/kalshi/KalshiScreen.cpp:3803-3817` (endpoint choice + subscribe frame) — **confined diff, duel-sensitive file**
- Modify: `src/services/crypto/CoinbaseEndpoints.h` (add WS constants + frame builders)
- Modify: `tests/tst_coinbase_endpoints.cpp` (frame-builder tests)

**Interfaces:**
- Produces (in `CoinbaseEndpoints.h`): `constexpr` `kAdvancedTradeWsUrl = "wss://advanced-trade-ws.coinbase.com"`, `kLegacyExchangeWsUrl = "wss://ws-feed.exchange.coinbase.com"` (kept ONLY as fallback), `QJsonObject advanced_ws_subscribe(const QStringList& product_ids, const QString& channel)` → `{"type":"subscribe","product_ids":[...],"channel":"level2"}` (Advanced Trade WS takes a SINGLE `channel` string, not the legacy `channels` array).

- [ ] **Step 1: Failing frame test** — assert `advanced_ws_subscribe({"BTC-USD"}, "level2")` has `type=="subscribe"`, `channel=="level2"` (STRING, and NO `channels` key), `product_ids==["BTC-USD"]`. Build → FAIL.

- [ ] **Step 2: Implement** the constants + builder in `CoinbaseEndpoints.h`. Test PASS; neuter (emit `channels` array) → FAIL → restore.

- [ ] **Step 3: CryptoLatencyService** — in `make_feed` (`:325`) point coinbase at `kAdvancedTradeWsUrl`; update its subscribe frame to `advanced_ws_subscribe(...)` and its message handler for the Advanced shape: events arrive as `{"channel":"l2_data","events":[{"type":"snapshot"|"update","product_id":...,"updates":[{"side":"bid"|"offer","price_level":"...","new_quantity":"..."}]}]}` — adapt whatever fields the latency measurement actually consumes (read the current handler first; it may only need message timestamps, in which case the parse change is trivial). Also subscribe `"heartbeats"` alongside to keep the connection alive on quiet books.

- [ ] **Step 4: KalshiScreen (confined + fallback).** In the endpoint pick at `:3803-3805`: try Advanced first, fall back to legacy on failure —

```cpp
    const QUrl endpoint(reference_dom_venue_ == QStringLiteral("coinbase")
                            ? QUrl(reference_dom_use_legacy_coinbase_
                                       ? services::crypto::kLegacyExchangeWsUrl
                                       : services::crypto::kAdvancedTradeWsUrl)
                            : QUrl(QStringLiteral("wss://ws.kraken.com/v2")));
```

New member `bool reference_dom_use_legacy_coinbase_ = false;` — set `true` (and log) in the socket-error/close handler when the venue is coinbase and the Advanced attempt never produced a book, so the next reconnect uses the legacy feed while it still answers. `subscribe_reference_dom()` (`:3809`) branches: Advanced → `advanced_ws_subscribe({product}, "level2")`; legacy branch keeps today's frame verbatim. The l2 message parse gains the Advanced `l2_data`-events shape next to the legacy `snapshot`/`l2update` shape (both handled; whichever arrives feeds the same `reference_dom_bids_/asks_`). **The entire diff in this file must touch only the reference-DOM block; nothing else.**

- [ ] **Step 5: Build + tests + commit.** Full build compiles; `ctest -R coinbase` PASS. Manual verification of the two WS paths happens at the duel-safe deployment window, NOT now — note that explicitly in the PR body. `git commit -am "feat(crypto): Advanced Trade WS for latency+Kalshi spot reference, legacy fallback"`

**Phase 1 PR:** open PR `feat/crypto-p1-coinbase-correctness` → main; body cites spec + this plan, states MERGED ≠ DEPLOYED.

---

# Phase 2 — Account WS + live ladder overlay (branch `feat/crypto-p2-account-ws`)

### Task 6: `ws_stream.py` authenticated account mode

**Files:**
- Modify: `scripts/exchange/ws_stream.py`
- Create: `tests/test_ws_stream_account.py`

**Interfaces:**
- Produces: new stdin command `{"cmd":"set_account_credentials","apiKey":"...","secret":"...","password":"..."}` — on receipt, spawns supervised account watchers emitting `{"type":"account_order",...}`, `{"type":"account_mytrade",...}`, `{"type":"account_balance",...}` lines. Credentials NEVER on argv/env. Task 7 consumes these line types.
- Emits unified-ccxt-shaped payloads (ccxt.pro normalizes Coinbase user channel):

```
{"type":"account_order","order":{"id":"..","symbol":"BTC/USD","side":"buy","type":"limit","price":118000.0,"amount":0.001,"filled":0.0,"remaining":0.001,"status":"open","timestamp":...}}
{"type":"account_mytrade","trade":{"id":"..","order":"..","symbol":"BTC/USD","side":"buy","price":...,"amount":...,"cost":...,"timestamp":...}}
{"type":"account_balance","balances":{"USD":{"free":..,"used":..,"total":..},...}}
```

- [ ] **Step 1: Failing test** — `tests/test_ws_stream_account.py` (unittest; import the module, no network):

```python
import json, sys, unittest
sys.path.insert(0, "scripts/exchange")
import ws_stream

class TestAccountMode(unittest.TestCase):
    def test_order_message_shape(self):
        order = {"id": "o1", "symbol": "BTC/USD", "side": "buy", "type": "limit",
                 "price": 118000.0, "amount": 0.001, "filled": 0.0,
                 "remaining": 0.001, "status": "open", "timestamp": 1753000000000}
        msg = ws_stream.make_account_order_msg(order)
        self.assertEqual(msg["type"], "account_order")
        self.assertEqual(msg["order"]["id"], "o1")
        self.assertEqual(msg["order"]["status"], "open")

    def test_balance_message_filters_zero(self):
        bal = {"USD": {"free": 100.0, "used": 0.0, "total": 100.0},
               "ETH": {"free": 0.0, "used": 0.0, "total": 0.0}}
        msg = ws_stream.make_account_balance_msg(bal)
        self.assertIn("USD", msg["balances"])
        self.assertNotIn("ETH", msg["balances"])   # zero balances filtered (daemon parity)

    def test_credentials_never_leave_process_boundary(self):
        # set_account_credentials must be a recognized stdin command constant,
        # and the module must not read account creds from argv or os.environ.
        self.assertIn("set_account_credentials", ws_stream.ACCOUNT_CMDS)
        src = open("scripts/exchange/ws_stream.py").read()
        self.assertNotIn("ACCOUNT_API_KEY", src)   # no env-var creds path

if __name__ == "__main__":
    unittest.main()
```

Run: `python3 -m unittest tests.test_ws_stream_account -v` → FAIL (`make_account_order_msg` missing).

- [ ] **Step 2: Implement.** In `ws_stream.py` add pure builders + watchers + stdin wiring:

```python
ACCOUNT_CMDS = {"set_account_credentials"}

def make_account_order_msg(order):
    keep = ("id","clientOrderId","symbol","side","type","price","amount",
            "filled","remaining","average","status","timestamp")
    return {"type": "account_order", "order": {k: order.get(k) for k in keep}}

def make_account_mytrade_msg(trade):
    keep = ("id","order","symbol","side","price","amount","cost","timestamp")
    return {"type": "account_mytrade", "trade": {k: trade.get(k) for k in keep}}

def make_account_balance_msg(balances):
    out = {}
    for cur, b in (balances or {}).items():
        if not isinstance(b, dict):
            continue
        if (b.get("total") or 0) or (b.get("free") or 0) or (b.get("used") or 0):
            out[cur] = {"free": b.get("free"), "used": b.get("used"), "total": b.get("total")}
    return {"type": "account_balance", "balances": out}
```

Watchers (each mirroring the existing error/backoff pattern, e.g. `watch_ticker` at line 226): `watch_account_orders` (`await exchange.watch_orders()` → emit per order), `watch_account_mytrades` (`watch_my_trades()`), `watch_account_balance` (`watch_balance()` → the raw dict's per-currency sub-dicts). Capability-gate each on `exchange.has["watchOrders"|"watchMyTrades"|"watchBalance"]`; emit `{"type":"info","message":"account channel <x> unsupported"}` when absent. In `stdin_command_loop` add:

```python
        elif action == "set_account_credentials":
            exchange.apiKey   = cmd.get("apiKey") or ""
            exchange.secret   = _normalize_pem(cmd.get("secret") or "")
            exchange.password = cmd.get("password") or ""
            if not state.get("account_tasks"):
                state["account_tasks"] = spawn_account_tasks(exchange, exchange_id)
                emit({"type": "status", "account": True, "exchange": exchange_id})
```

with `spawn_account_tasks` creating the three `supervise()`-wrapped tasks (skip unsupported), and `_normalize_pem` copied from `exchange_daemon.py`'s implementation (Coinbase CDP EC-PEM literal-`\n` fix — same function, same gating on `"-----BEGIN"`). Add `state["account_tasks"]` to the `finally:` teardown list in `main()`.

- [ ] **Step 3: Run tests** → PASS. Neuter (drop the zero-balance filter) → `test_balance_message_filters_zero` FAILS → restore.

- [ ] **Step 4: Commit** — `git add scripts/exchange/ws_stream.py tests/test_ws_stream_account.py && git commit -m "feat(crypto): authenticated account watchers in ws_stream.py (stdin-injected creds)"`

### Task 7: C++ account-stream plumbing → DataHub

**Files:**
- Modify: `src/trading/ExchangeSession.h` (`SessionPublisher` at `:39-46`; new members) and `src/trading/ExchangeSession.cpp` (`handle_ws_line`, `start_ws`, credential push)
- Modify: `src/trading/ExchangeSessionManager.cpp:81-111` (new topic publishers + patterns)
- Modify: `tests/tst_feed_reconnect.cpp` OR create `tests/tst_exchange_account_stream.cpp` (+ CMake registration) — a parse-level test on the new line handling

**Interfaces:**
- Consumes: Task 6's `account_order` / `account_mytrade` / `account_balance` JSON lines.
- Produces DataHub topics (consumed by Task 8/9/14): `ws:<exchange>:account_order:<pair>`, `ws:<exchange>:account_mytrade:<pair>`, `ws:<exchange>:account_balance` — payloads = the inner `order`/`trade`/`balances` QJsonObject, published via three new `SessionPublisher` std::functions `publish_account_order/mytrade/balance` (same seam style as `publish_ticker` at `ExchangeSession.h:42`). Also `ExchangeSession::push_account_credentials()` — writes the `set_account_credentials` stdin line to `ws_process_` iff credentials are non-empty; called from `start_ws` success path and from `set_credentials`.

- [ ] **Step 1: Failing test** — extract the line-dispatch into a testable seam if `handle_ws_line` is a private monolith: add a small pure free function in a new header `src/trading/AccountStreamParse.h`:

```cpp
#pragma once
#include <QJsonObject>
#include <QString>
namespace openmarketterminal::trading {
struct AccountLine { QString kind; QString symbol; QJsonObject payload; };
/// Classify a ws_stream.py JSON line: kind ∈ {"order","mytrade","balance",""}.
/// symbol is the unified pair for order/mytrade ("" for balance).
inline AccountLine parse_account_line(const QJsonObject& j) {
    const QString t = j.value(QStringLiteral("type")).toString();
    if (t == QLatin1String("account_order")) {
        const QJsonObject o = j.value(QStringLiteral("order")).toObject();
        return {QStringLiteral("order"), o.value(QStringLiteral("symbol")).toString(), o};
    }
    if (t == QLatin1String("account_mytrade")) {
        const QJsonObject o = j.value(QStringLiteral("trade")).toObject();
        return {QStringLiteral("mytrade"), o.value(QStringLiteral("symbol")).toString(), o};
    }
    if (t == QLatin1String("account_balance"))
        return {QStringLiteral("balance"), QString(), j.value(QStringLiteral("balances")).toObject()};
    return {QString(), QString(), {}};
}
} // namespace openmarketterminal::trading
```

Test (`tests/tst_exchange_account_stream.cpp`, links `Qt6::Core Qt6::Test` only): feed the three Task-6 payload shapes + a `{"type":"ticker"}` line → assert kind/symbol/payload routing and that ticker lines classify as `kind==""` (untouched by the account path). Build → FAIL → implement → PASS → neuter (swap order/mytrade kinds) → FAIL → restore.

- [ ] **Step 2: Wire the session.** In `ExchangeSession::handle_ws_line`, before the existing type dispatch, call `parse_account_line`; when `kind` non-empty, invoke the matching new publisher (with `remap_symbol(symbol)` applied, same as public data) and return. Add the three `std::function` members to `SessionPublisher` (`:42-46`). In `ExchangeSessionManager.cpp:81-99` add the three publisher lambdas mirroring the ticker one — topic strings exactly as in Interfaces above. `topic_patterns()` (`:108`) already whitelists `ws:<id>:*` so no pattern change is needed — verify by reading `:141` and note it in the commit message.

- [ ] **Step 3: Credentials push.** Implement `push_account_credentials()`: read `credentials_` under the session mutex; if the key is empty do nothing; else write one JSON line (`QJsonDocument::toJson(Compact) + "\n"`) to `ws_process_`. Call from (a) end of successful `start_ws`, (b) `set_credentials` when `ws_process_` is running. SECURITY: never log the line — log only `"account credentials pushed to ws stream"`.

- [ ] **Step 4: Build + tests + commit** — `git commit -am "feat(crypto): account order/trade/balance stream published to DataHub"`

### Task 8: Screen consumes account stream (instant blotter + cadence relax)

**Files:**
- Create: `src/screens/crypto_trading/CryptoAccountCadence.h`
- Create: `tests/tst_crypto_account_cadence.cpp` (+ CMake, pure header pattern)
- Modify: `src/screens/crypto_trading/CryptoTradingScreen.h` (members), `CryptoTradingScreen.cpp` (`hub_subscribe_topics`), `CryptoTradingScreen_Handlers.cpp:295` (timer start), `CryptoTradingScreen_Refresh.cpp:533` (`refresh_live_data`)

**Interfaces:**
- Consumes: Task 7 topics; existing `refresh_live_data()` REST cycle.
- Produces: `crypto::account_poll_interval_ms(qint64 now_ms, qint64 last_account_ws_event_ms)` — the single cadence authority.

- [ ] **Step 1: Failing cadence test:**

```cpp
#include "screens/crypto_trading/CryptoAccountCadence.h"
#include <QtTest/QtTest>
using namespace openmarketterminal::crypto;
class TstCryptoAccountCadence : public QObject {
    Q_OBJECT
  private slots:
    void baseline_when_no_ws() { QCOMPARE(account_poll_interval_ms(1'000'000, 0), 5000); }
    void relaxed_when_ws_fresh() { QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 9'000), 30000); }
    void snaps_back_on_staleness() { QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 16'000), 5000); }
    void boundary_is_fifteen_seconds() {
        QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 15'000), 5000); // >= 15s stale → baseline
        QCOMPARE(account_poll_interval_ms(1'000'000, 1'000'000 - 14'999), 30000);
    }
};
QTEST_MAIN(TstCryptoAccountCadence)
#include "tst_crypto_account_cadence.moc"
```

Build → FAIL.

- [ ] **Step 2: Implement:**

```cpp
#pragma once
// REST account-poll cadence: relax to 30s ONLY while the authenticated WS
// stream has delivered an event in the last 15s; otherwise (never/stale)
// stay at today's 5s baseline. Fail toward MORE polling, never less.
#include <QtGlobal>
namespace openmarketterminal::crypto {
inline int account_poll_interval_ms(qint64 now_ms, qint64 last_account_ws_event_ms) {
    constexpr int kBaselineMs = 5000, kRelaxedMs = 30000;
    constexpr qint64 kWsFreshWindowMs = 15000;
    if (last_account_ws_event_ms <= 0) return kBaselineMs;
    return (now_ms - last_account_ws_event_ms < kWsFreshWindowMs) ? kRelaxedMs : kBaselineMs;
}
} // namespace openmarketterminal::crypto
```

Test PASS; neuter (always return 30000) → FAIL → restore.

- [ ] **Step 3: Subscribe + react.** New members: `qint64 last_account_ws_event_ms_ = 0;`. In `hub_subscribe_topics()` add subscriptions for the three account topics of the active exchange (mirror the existing public-topic subscribe calls in that function — read it first for the exact DataHub API). Handler behavior: any account event → stamp `last_account_ws_event_ms_`; `account_order` → update blotter immediately via existing `bottom_panel_->set_live_orders(...)` shape (accumulate into a `QHash<QString,QJsonObject> live_orders_by_id_`, drop `status ∈ {"closed","canceled","filled","rejected","expired"}` after forwarding once, emit the values as a QJsonArray) + schedule ONE confirming `refresh_live_data()` via a 300ms single-shot (coalesced by the existing `live_inflight_` guard — REST remains source of truth); `account_balance` → recompute the AVAIL display through the same currency-fallback chain used in `_AsyncFetch.cpp:128-144` (extract that chain into a small shared static helper in the screen rather than duplicating it); `account_mytrade` → forward to Task 9's avg-entry accumulator and Task 14's notifier seam (both no-ops until those tasks land).
- [ ] **Step 4: Cadence application.** In `refresh_live_data()` head (`Refresh.cpp:533`): `live_data_timer_->setInterval(crypto::account_poll_interval_ms(QDateTime::currentMSecsSinceEpoch(), last_account_ws_event_ms_));` — the timer re-evaluates every tick, so WS death snaps back within one interval. Mode-toggle start (`Handlers.cpp:295`) stays `start(5000)`.
- [ ] **Step 5: Build + full crypto ctest + commit** — `git commit -am "feat(crypto): live blotter reacts to account WS; REST cadence relaxes only while WS fresh"`

### Task 9: Live ladder overlay (own orders + est. avg entry)

**Files:**
- Create: `src/screens/crypto_trading/CryptoLiveOverlay.h`
- Create: `tests/tst_crypto_live_overlay.cpp` (+ CMake, `Qt6::Core Qt6::Test`)
- Modify: `CryptoTradingScreen.h` (member + method), `CryptoTradingScreen_Refresh.cpp` (feed ladder in live mode), `CryptoTradingScreen_Handlers.cpp:298-305` (remove the always-clear, keep clear-on-mode-ENTRY)

**Interfaces:**
- Consumes: Task 8's `live_orders_by_id_` (unified order QJsonObjects) and `account_mytrade` events; existing `crypto::MyOrder` + `CryptoLadder::set_my_orders/set_avg_entry` (usage exactly as `Handlers.cpp:304-305`).
- Produces: `crypto::live_orders_to_my_orders(const QVector<QJsonObject>& unified_orders, const QString& symbol) -> QVector<MyOrder>`; `class LiveAvgEntry { void add_trade(side, price, amount); double avg_entry() const; double net_qty() const; void reset(); }` (VWAP of net open quantity; returns 0 avg when `net_qty() <= 0`).

- [ ] **Step 1: Failing tests** (all pure):

```cpp
#include "screens/crypto_trading/CryptoLiveOverlay.h"
#include "screens/crypto_trading/CryptoLadderModel.h"
#include <QtTest/QtTest>
using namespace openmarketterminal::crypto;

class TstCryptoLiveOverlay : public QObject {
    Q_OBJECT
    static QJsonObject ord(const char* sym, const char* side, double px, double remaining, const char* status) {
        return QJsonObject{{"symbol", sym}, {"side", side}, {"price", px},
                           {"remaining", remaining}, {"status", status}, {"type", "limit"}};
    }
  private slots:
    void filters_symbol_status_and_market_orders() {
        const QVector<QJsonObject> in = {
            ord("BTC/USD", "buy", 118000, 0.001, "open"),
            ord("ETH/USD", "buy", 3000, 1.0, "open"),        // other symbol → dropped
            ord("BTC/USD", "sell", 119000, 0.002, "open"),
            ord("BTC/USD", "buy", 117000, 0.001, "canceled"),// non-open → dropped
            QJsonObject{{"symbol","BTC/USD"},{"side","buy"},{"price",0.0},
                        {"remaining",0.001},{"status","open"},{"type","market"}}, // no price → dropped
        };
        const auto out = live_orders_to_my_orders(in, "BTC/USD");
        QCOMPARE(out.size(), 2);
        QVERIFY(out[0].is_buy);  QCOMPARE(out[0].price, 118000.0); QCOMPARE(out[0].qty, 0.001);
        QVERIFY(!out[1].is_buy); QCOMPARE(out[1].price, 119000.0);
    }
    void vwap_avg_entry() {
        LiveAvgEntry a;
        a.add_trade("buy", 100.0, 1.0);
        a.add_trade("buy", 110.0, 1.0);
        QCOMPARE(a.net_qty(), 2.0);
        QCOMPARE(a.avg_entry(), 105.0);
        a.add_trade("sell", 120.0, 1.0);          // reduce → avg of remaining unchanged
        QCOMPARE(a.net_qty(), 1.0);
        QCOMPARE(a.avg_entry(), 105.0);
        a.add_trade("sell", 120.0, 1.5);          // net flips ≤ 0 → no marker
        QVERIFY(a.net_qty() <= 0.0);
        QCOMPARE(a.avg_entry(), 0.0);
    }
    void reset_clears() {
        LiveAvgEntry a; a.add_trade("buy", 100.0, 1.0); a.reset();
        QCOMPARE(a.net_qty(), 0.0); QCOMPARE(a.avg_entry(), 0.0);
    }
};
QTEST_MAIN(TstCryptoLiveOverlay)
#include "tst_crypto_live_overlay.moc"
```

Build → FAIL.

- [ ] **Step 2: Implement `CryptoLiveOverlay.h`** — `live_orders_to_my_orders`: keep `status=="open"` (and `"partially_filled"` if ccxt emits it — accept both), `symbol` match, `price > 0`, `remaining > 0`; map side via `side=="buy"`; qty = `remaining`. `LiveAvgEntry`: running `net_qty_` + `cost_basis_`; buy → both increase; sell → `cost_basis_ -= avg_entry()*amount` then `net_qty_ -= amount`; if `net_qty_ <= 0` → zero both (flat/flipped ⇒ marker off; a genuine short book is out of scope for the est. marker and stays markerless — document in the header comment). `avg_entry()` returns `net_qty_ > 0 ? cost_basis_/net_qty_ : 0.0`.

- [ ] **Step 3: Run** → PASS; neuter (drop the status filter) → first test FAILS → restore.

- [ ] **Step 4: Wire.** Members: `LiveAvgEntry live_avg_entry_;`. On `account_mytrade` (Task 8 handler): if trade symbol == `selected_symbol_`, `live_avg_entry_.add_trade(side, price, amount)`. On any `account_order`/blotter refresh in Live mode: `ladder_->set_my_orders(live_orders_to_my_orders(live_orders_values_, selected_symbol_)); ladder_->set_avg_entry(live_avg_entry_.avg_entry());`. Seed `LiveAvgEntry` on entering Live mode / symbol switch from the my-trades REST fetch (`_AsyncFetch.cpp` my_trades completion → replay rows into a fresh accumulator, oldest-first) so the marker is right without waiting for new fills; `reset()` on symbol/exchange change. In `Handlers.cpp:298-305` replace the unconditional overlay-clear comment block: still clear on mode ENTRY (stale-paper-overlay bug stays fixed), then let the live feed repopulate — update the comment to say Live overlay is now wired via CryptoLiveOverlay.
- [ ] **Step 5: Build + full crypto ctest + commit** — `git commit -am "feat(crypto): live DOM ladder overlay — own resting orders + est avg entry"`

**Phase 2 PR** — same merged≠deployed note.

---

# Phase 3 — Honest paper fills (branch `feat/crypto-p3-honest-paper`)

### Task 10: `CryptoPaperFill` pure core

**Files:**
- Create: `src/screens/crypto_trading/CryptoPaperFill.h`
- Create: `tests/tst_crypto_paper_fill.cpp` (+ CMake, `Qt6::Core Qt6::Test`)

**Interfaces:**
- Consumes: `trading::OrderBookData` (`TradingTypes.h`: `bids/asks` as `QVector<QPair<double,double>>` price/size).
- Produces:

```cpp
struct PaperFillVerdict {
    bool ok = false;          // false → REJECT, show reason, place no order
    QString reason;           // human-visible rejection reason
    double fill_price = 0;    // size-weighted walk price incl. nothing else
    double filled_qty = 0;    // may be < requested (partial: thin book)
    double fee_paid = 0;      // taker fee on filled notional
};
PaperFillVerdict paper_market_fill(const QString& side, double qty,
                                   const trading::OrderBookData& book,
                                   qint64 book_age_ms, double taker_fee_rate);
```

- [ ] **Step 1: Failing tests:**

```cpp
#include "screens/crypto_trading/CryptoPaperFill.h"
#include <QtTest/QtTest>
using namespace openmarketterminal::crypto;
using trading::OrderBookData;

static OrderBookData book2() {   // asks: 100.0×1.0 then 101.0×2.0; bids: 99.0×1.0, 98.0×2.0
    OrderBookData b; b.symbol = "BTC/USD";
    b.asks = {{100.0, 1.0}, {101.0, 2.0}};
    b.bids = {{99.0, 1.0}, {98.0, 2.0}};
    b.best_bid = 99.0; b.best_ask = 100.0;
    return b;
}

class TstCryptoPaperFill : public QObject {
    Q_OBJECT
  private slots:
    void buy_walks_the_asks() {
        const auto v = paper_market_fill("buy", 2.0, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.filled_qty, 2.0);
        QCOMPARE(v.fill_price, (100.0 * 1.0 + 101.0 * 1.0) / 2.0);   // 100.5 size-weighted
        QCOMPARE(v.fee_paid, 2.0 * 100.5 * 0.004);
    }
    void sell_walks_the_bids() {
        const auto v = paper_market_fill("sell", 1.5, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.fill_price, (99.0 * 1.0 + 98.0 * 0.5) / 1.5);
    }
    void partial_on_thin_book() {
        const auto v = paper_market_fill("buy", 10.0, book2(), 1000, 0.004);
        QVERIFY(v.ok);
        QCOMPARE(v.filled_qty, 3.0);                                  // all visible ask depth
        QVERIFY(v.reason.contains("partial"));
    }
    void stale_book_rejects() {
        const auto v = paper_market_fill("buy", 1.0, book2(), 5001, 0.004);
        QVERIFY(!v.ok);
        QVERIFY(v.reason.contains("stale"));
    }
    void empty_book_rejects() {
        const auto v = paper_market_fill("buy", 1.0, OrderBookData{}, 100, 0.004);
        QVERIFY(!v.ok);
    }
    void bad_qty_rejects() {
        QVERIFY(!paper_market_fill("buy", 0.0, book2(), 100, 0.004).ok);
        QVERIFY(!paper_market_fill("buy", -1.0, book2(), 100, 0.004).ok);
    }
};
QTEST_MAIN(TstCryptoPaperFill)
#include "tst_crypto_paper_fill.moc"
```

Build → FAIL.

- [ ] **Step 2: Implement** — header-only. Staleness threshold `constexpr qint64 kMaxBookAgeMs = 5000;` (same 5s the sandbox `data_quality_from_freshness` uses — cite `services/sandbox/FreshnessGate.h` in the comment). Walk the opposite side best-first, accumulate size-weighted notional, cap at available depth (partial ⇒ `reason = "partial fill: book depth ..."`), reject on: qty ≤ 0 / non-finite, empty side, `book_age_ms > kMaxBookAgeMs` (`reason = "stale book (...ms) — paper fill refused"`). Fee on filled notional only.

- [ ] **Step 3: Run** → PASS. Neuter TWO ways (each restores before the next): (a) skip the staleness check → `stale_book_rejects` FAILS; (b) fill everything at `best_ask` instead of walking → `buy_walks_the_asks` FAILS.

- [ ] **Step 4: Commit** — `git commit -m "feat(crypto): pure walk-the-book paper fill core with stale-book rejection"`

### Task 11: Wire honest fills into the paper order path

**Files:**
- Modify: `src/screens/crypto_trading/CryptoTradingScreen_Handlers.cpp:371-392` (paper branch of `on_order_submitted`)
- Modify: `src/screens/crypto_trading/CryptoTradingScreen.h` (track book freshness)

**Interfaces:**
- Consumes: Task 10 `paper_market_fill`; existing `pending_orderbook_` / `has_pending_orderbook_` (`CryptoTradingScreen.h:219-220`), `pt_place_order`/`pt_fill_order`, `order_entry_->show_order_result(bool, QString)` (usage as in the live branch), fee settings (locate the taker-fee read used by the Fees tab — grep `fee` in `CryptoTradingScreen_AsyncFetch.cpp` / settings; fall back to a `0.004` constant with a `// TODO(fees)` ONLY if no setting exists — and then add the setting read, do not ship the TODO).

- [ ] **Step 1:** Track book freshness: new members `trading::OrderBookData last_book_; qint64 last_book_ms_ = 0;` — stamp both wherever `pending_orderbook_` is assigned from the hub (grep `has_pending_orderbook_ = true` for the assignment sites; also stamp in `flush_ws_updates` when it forwards to `orderbook_`).

- [ ] **Step 2:** Replace the dishonest market-fill block (`Handlers.cpp:371-392`). Current code fills at `ticker.last` or **literal 1000.0**; new code:

```cpp
        if (trading_mode_ == TradingMode::Paper) {
            if (order_type == "market") {
                const qint64 age_ms = last_book_ms_ > 0
                    ? QDateTime::currentMSecsSinceEpoch() - last_book_ms_ : std::numeric_limits<qint64>::max();
                const auto verdict = crypto::paper_market_fill(side, qty, last_book_, age_ms, paper_taker_fee_rate());
                if (!verdict.ok) {
                    order_entry_->show_order_result(false, tr("PAPER reject: %1").arg(verdict.reason));
                    return;   // REJECTED — no order row, honest refusal
                }
                auto order = pt_place_order(portfolio_id_, selected_symbol_, side, order_type,
                                            verdict.filled_qty, verdict.fill_price, std::nullopt);
                pt_fill_order(order.id, verdict.fill_price);
                order_entry_->show_order_result(true,
                    tr("PAPER filled %1 @ %2 (fee %3)%4")
                        .arg(verdict.filled_qty).arg(verdict.fill_price).arg(verdict.fee_paid)
                        .arg(verdict.filled_qty < qty ? tr(" — PARTIAL: book depth") : QString()));
            } else {
                // limit/stop path unchanged: pt_place_order + OrderMatcher (maker model)
                ...existing code verbatim...
            }
            refresh_portfolio();
        }
```

`paper_taker_fee_rate()` = small private helper reading the screen's existing fee configuration (find it first; the Fees tab + `pt_create_portfolio(..., 0.001, exch)` at `CryptoTradingScreen.cpp:688` show a fee rate already flows — reuse THAT value's source so paper fees stay consistent with the blotter). Record `verdict.fee_paid` the same way the current pt flow records fees (check `pt_fill_order` — if it derives fee from the portfolio's stored rate, do NOT double-count; pass the rate, not the fee).

- [ ] **Step 3: Build + manual-logic check** — full build; run `ctest -R "crypto"` → PASS. Grep gate: `grep -n "1000.0" src/screens/crypto_trading/CryptoTradingScreen_Handlers.cpp` → zero hits in the paper path.

- [ ] **Step 4: Commit** — `git commit -am "feat(crypto): paper market orders walk the live book; stale book rejects honestly"`

**Phase 3 PR.**

---

# Phase 4 — Alerts (branch `feat/crypto-p4-alerts`)

### Task 12: `CryptoAlertEngine` pure core

**Files:**
- Create: `src/screens/crypto_trading/CryptoAlertEngine.h`
- Create: `tests/tst_crypto_alert_engine.cpp` (+ CMake, `Qt6::Core Qt6::Test`)

**Interfaces:**
- Produces:

```cpp
struct CryptoAlert {
    QString id;               // uuid string
    QString exchange, symbol;
    QString kind;             // "price_cross_up" | "price_cross_down" | "spread_bps"
    double threshold = 0;     // price, or bps for spread
    bool armed = true;
    QJsonObject to_json() const; static CryptoAlert from_json(const QJsonObject&);
};
class CryptoAlertEngine {
    void set_alerts(const QVector<CryptoAlert>&); QVector<CryptoAlert> alerts() const;
    // Returns alerts that FIRE on this tick (and disarms them in place).
    QVector<CryptoAlert> on_tick(const QString& exchange, const QString& symbol,
                                 double last, double bid, double ask);
    void rearm(const QString& id);
};
```

- [ ] **Step 1: Failing tests** — cross-up fires once at `last >= threshold` from below (requires a prior tick below: seed with first tick, no fire on the very first observation), cross-down mirrors, spread fires when `(ask-bid)/mid*10000 >= threshold`, fired alert is disarmed (second qualifying tick → no fire), `rearm` restores, wrong exchange/symbol never fires, `to_json/from_json` round-trips all fields including `armed=false`. Build → FAIL.

- [ ] **Step 2: Implement** — engine keeps `QHash<QString,double> last_price_;` keyed `exchange:symbol` for cross detection (first tick only seeds). Disarm-in-place on fire. Pure Qt-core only.

- [ ] **Step 3: Run** → PASS. Neuter (fire on first observation without a prior-side check) → cross test FAILS → restore.

- [ ] **Step 4: Commit** — `git commit -m "feat(crypto): pure alert engine (price cross, spread bps, disarm/rearm)"`

### Task 13: Alert UI + persistence + evaluation

**Files:**
- Modify: `src/screens/crypto_trading/CryptoWatchlist.{h,cpp}` (context-menu entry) — read its existing context menu first; if none exists, add one with this single action
- Modify: `CryptoTradingScreen.h/.cpp` (engine member, persistence, evaluation in `flush_ws_updates`)

**Interfaces:**
- Consumes: Task 12 engine; `SettingsRepository` (persist under key `crypto.alerts` as a QJsonArray string — mirror how `MarketsScreen` persists panel layout; grep `SettingsRepository` usage there for the exact API); `NotificationService::instance().send(NotificationRequest)` with `NotifTrigger::PriceAlert`.

- [ ] **Step 1:** Watchlist context menu: right-click row → "Alert at price…" → `QInputDialog::getDouble` (prefill = row's last price, 8 decimals) → emit new signal `alert_requested(QString symbol, double price)`. Screen connects: kind = `price_cross_up` if threshold > current last else `price_cross_down`; append to engine + persist.
- [ ] **Step 2:** Persistence: load `crypto.alerts` in the ctor (after `setup_ui`), save on every add/remove/fire (fire persists the disarmed state — an alert that fired while the app was closed must not re-fire on restart from a stale armed flag).
- [ ] **Step 3:** Evaluation: in `flush_ws_updates`, after the primary-ticker flush block, run `alert_engine_.on_tick(exchange_id_, t.symbol, t.last, t.bid, t.ask)` for the primary + each `pending_tickers_` entry; each fired alert → `NotificationRequest{tr("Price alert: %1").arg(a.symbol), tr("%1 crossed %2").arg(a.symbol).arg(a.threshold), NotifLevel::Alert, NotifTrigger::PriceAlert}` → `NotificationService::instance().send(req)`. Local only — no provider config changes.
- [ ] **Step 4:** Build + crypto ctest sweep; manual smoke deferred to deployment window. Commit — `git commit -am "feat(crypto): price/spread alerts — watchlist UI, persistence, WS-tick evaluation"`

### Task 14: Fill/cancel notifications from the account stream

**Files:**
- Create: `src/screens/crypto_trading/CryptoFillNotifier.h`
- Create: `tests/tst_crypto_fill_notifier.cpp` (+ CMake, `Qt6::Core Qt6::Test`)
- Modify: `CryptoTradingScreen.h/.cpp` (instantiate; call from the Task-8 `account_order` handler)

**Interfaces:**
- Consumes: Task 8's `account_order` unified payloads (WS) AND the REST open-orders refresh (both paths can report the same transition — dedupe is the point).
- Produces: `class CryptoFillNotifier { /// Returns a message to notify, or empty if suppressed (dedupe/uninteresting). QString on_order_event(const QJsonObject& unified_order); }` — notifies on transitions to `filled` / `canceled` / `rejected`, once per (order id, terminal status); `open`/`partially_filled` are silent.

- [ ] **Step 1: Failing test** — same order id + `status:"filled"` twice → first call returns non-empty (contains symbol, side, "filled"), second returns empty; `status:"open"` → empty; distinct ids both notify; `canceled` notifies once; missing id → empty (never notify unidentifiable orders). Build → FAIL.
- [ ] **Step 2: Implement** — `QSet<QString> seen_;` keyed `id + ":" + status` for terminal statuses only; message `"%1 %2 %3 %4 @ %5"` (side upper, filled qty, symbol, status, average-or-price). Bound memory: clear `seen_` when it exceeds 4096 entries.
- [ ] **Step 3: Run** → PASS; neuter (drop dedupe) → FAIL → restore.
- [ ] **Step 4: Wire:** in the Task-8 `account_order` handler AND the REST orders-refresh completion, Live mode only: `const QString msg = fill_notifier_.on_order_event(order); if (!msg.isEmpty()) NotificationService::instance().send({tr("Coinbase order"), msg, NotifLevel::Info, NotifTrigger::OrderFill});`. Paper stays silent (spec default).
- [ ] **Step 5: Build + ctest + commit** — `git commit -am "feat(crypto): deduped fill/cancel notifications from account stream"`

**Phase 4 PR.**

---

## Self-review record

- **Spec coverage:** P1-1→T1/T2, P1-2→T3, P1-3→T4/T5, P2-1→T6/T7/T8, P2-2→T9, P3→T10/T11 (deviation #3 documented), P4→T12/T13/T14. Spec's "AVAIL label" fix lands in T2 (root cause) + T8 (balance path reuse). Spec non-goals honored: no ServeCommand/advisor edits (T5's KalshiScreen exception is fallback-guarded + diff-confined); no execution-tool changes; no Markets.
- **Deferred by design:** live WS empirical verification of T5/T6 endpoints against real Coinbase happens at the duel-safe deployment window; each PR body must carry the merged≠deployed banner.
- **Type consistency check:** `MyOrder{price,qty,is_buy}` matches `CryptoLadderModel.h`; `OrderBookData` bids/asks as `QVector<QPair<double,double>>` matches `TradingTypes.h`; `NotificationRequest{title,message,level,trigger}` matches `NotificationService.h`; publisher seam mirrors `SessionPublisher` std::function style at `ExchangeSession.h:42`.
