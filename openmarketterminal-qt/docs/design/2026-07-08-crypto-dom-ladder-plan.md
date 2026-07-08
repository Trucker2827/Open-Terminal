# Crypto DOM Price Ladder (Phase 1, read-only) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only Kraken-style fixed-price-tick DOM ladder panel (`ORDERS | BID | PRICE | ASK | VAP`) to the crypto trading screen, reusing the existing WS orderbook / trades / orders feeds.

**Architecture:** A pure, unit-tested `CryptoLadderModel` (bucketing + session VAP + row-window build + orders/position overlay) does all the math; a paint-based `CryptoLadder` QWidget owns a model, exposes setters the screen feeds from its existing DataHub/refresh handlers, and paints the rows + VAP heatmap. No new data plumbing; no order-mutation code.

**Tech Stack:** Qt6/C++17, custom `paintEvent` rendering (mirrors `CryptoOrderBook`), QtTest+ctest. Tests build under `/tmp/ot-build-test` with `-DOPENMARKETTERMINAL_BUILD_TESTS=ON`; GUI builds under `openmarketterminal-qt/build`.

## Global Constraints

- **READ-ONLY. Phase 1 ships ZERO order-mutation code** — no `place_order` / `cancel_order` / `UnifiedTrading` / click-to-trade. The ladder can only display. A reviewer must be able to confirm it cannot place or cancel an order.
- Do NOT modify `CryptoOrderBook` — the ladder is additive, a separate panel.
- No new WS/daemon feeds — reuse the `OrderBookData` and trade feeds the crypto screen already subscribes to.
- **VAP accumulates at a fine base resolution** (the smallest offered grouping), re-aggregated to the display grouping at build time, so changing grouping never corrupts VAP. Every display grouping is an integer multiple of the base.
- Every fix ships a neuter-verified regression test (confirm it FAILS without the change). `assert()` is a no-op under NDEBUG — use QtTest `QVERIFY`/`QCOMPARE`.
- Stale-object trap: after restoring a neutered header via copy, `touch` it before rebuilding so Ninja recompiles.
- Surgical edits; match `CryptoOrderBook`/`CryptoTimeSales` style.

---

### Task 1: `CryptoLadderModel` — bucketing + depth row window (pure)

**Files:**
- Create: `src/screens/crypto_trading/CryptoLadderModel.h`
- Test: `tests/tst_crypto_ladder_model.cpp` (new; register in `tests/CMakeLists.txt`)

**Interfaces:**
- Produces (structs + functions consumed by later tasks + the widget):
```cpp
namespace openmarketterminal::crypto {
struct LadderRow {
    double price = 0;
    double bid_size = 0;
    double ask_size = 0;
    double vap = 0;
    double my_bid_qty = 0;
    double my_ask_qty = 0;
    bool   is_avg_entry = false;
};
struct LadderView {
    QVector<LadderRow> rows; // top (highest price) -> bottom (lowest)
    double mid = 0;
    double max_depth = 0;
    double max_vap = 0;
};
struct MyOrder { double price = 0; double qty = 0; bool is_buy = false; };

/// Snap a price down to its bucket at increment `grouping` (grouping > 0).
double bucket_price(double price, double grouping);
}
```
This task implements `bucket_price` and the depth-only part of `build` (VAP/overlays added in Tasks 2–3). The `build` signature is finalized here and reused verbatim later:
```cpp
class CryptoLadderModel {
  public:
    // depth: raw {price,size} levels; grouping: display increment; rows_each_side:
    // rows above and below mid. VAP/orders/avg default-empty until Tasks 2-3.
    LadderView build(const QVector<QPair<double,double>>& bids,
                     const QVector<QPair<double,double>>& asks,
                     double grouping, int rows_each_side) const;
};
```

- [ ] **Step 1: Write the failing test** — `tests/tst_crypto_ladder_model.cpp`:
```cpp
#include "screens/crypto_trading/CryptoLadderModel.h"
#include <QtTest>
using namespace openmarketterminal::crypto;

class TestCryptoLadderModel : public QObject {
    Q_OBJECT
  private slots:
    void bucketsPrices();
    void buildBucketsDepthAroundMid();
};

void TestCryptoLadderModel::bucketsPrices() {
    QCOMPARE(bucket_price(62617.3, 10.0), 62610.0);
    QCOMPARE(bucket_price(62610.0, 10.0), 62610.0);
    QCOMPARE(bucket_price(3.27, 0.1), 3.2); // fuzzy-safe multiple
    QVERIFY(qAbs(bucket_price(3.27, 0.1) - 3.2) < 1e-9);
}

void TestCryptoLadderModel::buildBucketsDepthAroundMid() {
    // bids below, asks above; two raw levels in the 62,600 bucket sum.
    QVector<QPair<double,double>> bids{{62599, 2.0}, {62595, 3.0}, {62580, 1.0}};
    QVector<QPair<double,double>> asks{{62601, 1.0}, {62603, 4.0}, {62620, 2.0}};
    CryptoLadderModel m;
    const auto v = m.build(bids, asks, 10.0, 2);
    // mid = (best_bid 62599 + best_ask 62601)/2 = 62600
    QVERIFY(qAbs(v.mid - 62600.0) < 1e-6);
    // rows are 2 each side of the 62600 bucket, top->bottom descending price
    QCOMPARE(v.rows.size(), 5); // 62620,62610,62600,62590,62580
    QCOMPARE(v.rows.first().price, 62620.0);
    QCOMPARE(v.rows.last().price, 62580.0);
    // 62600 bucket: asks 62601(1)+62603(4)=5 on the ask side; no bid there
    auto row600 = std::find_if(v.rows.begin(), v.rows.end(),
                               [](const LadderRow& r){ return qAbs(r.price-62600.0)<1e-6; });
    QVERIFY(row600 != v.rows.end());
    QCOMPARE(row600->ask_size, 5.0);
    QCOMPARE(row600->bid_size, 0.0);
    // 62590 bucket has bids 62599? no -> 62599 buckets to 62590: 2.0; 62595->62590: 3.0 => 5.0
    auto row590 = std::find_if(v.rows.begin(), v.rows.end(),
                               [](const LadderRow& r){ return qAbs(r.price-62590.0)<1e-6; });
    QCOMPARE(row590->bid_size, 5.0);
    QCOMPARE(v.max_depth, 5.0);
}
```
Register in `tests/CMakeLists.txt` as an APPLESS test (copy the `tst_portfolio_agent_filter` block: `Qt6::Core Qt6::Test`, `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/src)`).

- [ ] **Step 2: Run test to verify it fails** — configure if needed, then
`cmake --build /tmp/ot-build-test --target tst_crypto_ladder_model && /tmp/ot-build-test/tests/tst_crypto_ladder_model`
Expected: FAIL (header/functions undefined). The `ld: warning ... macOS-13.0 ... newer version 14.0` linker warnings are benign/pre-existing.

- [ ] **Step 3: Implement** `CryptoLadderModel.h` — the structs above plus:
```cpp
inline double bucket_price(double price, double grouping) {
    if (grouping <= 0) return price;
    return std::floor(price / grouping + 1e-9) * grouping;
}

class CryptoLadderModel {
  public:
    LadderView build(const QVector<QPair<double,double>>& bids,
                     const QVector<QPair<double,double>>& asks,
                     double grouping, int rows_each_side) const {
        LadderView v;
        if (grouping <= 0 || rows_each_side <= 0)
            return v;
        double best_bid = 0, best_ask = 0;
        for (const auto& b : bids) best_bid = std::max(best_bid, b.first);
        for (const auto& a : asks) best_ask = (best_ask == 0) ? a.first : std::min(best_ask, a.first);
        if (best_bid <= 0 || best_ask <= 0)
            return v;
        v.mid = (best_bid + best_ask) / 2.0;
        const double mid_bucket = bucket_price(v.mid, grouping);

        QHash<double,double> bid_by_bucket, ask_by_bucket;
        for (const auto& b : bids) bid_by_bucket[bucket_price(b.first, grouping)] += b.second;
        for (const auto& a : asks) ask_by_bucket[bucket_price(a.first, grouping)] += a.second;

        // rows_each_side above and below the mid bucket, plus the mid bucket
        for (int i = rows_each_side; i >= -rows_each_side; --i) {
            LadderRow r;
            r.price = mid_bucket + i * grouping;
            r.bid_size = bid_by_bucket.value(r.price, 0.0);
            r.ask_size = ask_by_bucket.value(r.price, 0.0);
            v.max_depth = std::max({v.max_depth, r.bid_size, r.ask_size});
            v.rows.append(r);
        }
        return v;
    }
};
```
(Note: a bucket at/above mid keeps only its ask side and a bucket below keeps its bid side naturally, because bids never land in above-mid buckets and vice versa for a crossed-free book; the test's 62600 bucket shows asks-only, 62590 bids-only.)

- [ ] **Step 4: Run test to verify it passes** — rebuild + run. Expected: PASS.

- [ ] **Step 5: Neuter-verify** — change `bucket_price`'s `std::floor` to `std::round`; confirm `bucketsPrices` FAILS; restore + `touch`.

- [ ] **Step 6: Commit**
```bash
git add src/screens/crypto_trading/CryptoLadderModel.h tests/tst_crypto_ladder_model.cpp tests/CMakeLists.txt
git commit -m "feat(crypto): CryptoLadderModel — price bucketing + depth row window"
```

---

### Task 2: Session VAP accumulator (fine base) + re-aggregation

**Files:**
- Modify: `src/screens/crypto_trading/CryptoLadderModel.h`
- Test: `tests/tst_crypto_ladder_model.cpp` (extend)

**Interfaces:**
- Consumes: Task 1's `build`, `LadderRow`.
- Produces: `void accumulate_vap(double price, double amount)`, `void reset_vap()`, and a `vap_base_` fine resolution; `build` now fills `LadderRow::vap` and `LadderView::max_vap`. New `build` overload adds no params — VAP is model state. The constructor takes the fine base:
```cpp
explicit CryptoLadderModel(double vap_base = 0.1); // fine resolution for VAP keying
```

- [ ] **Step 1: Write the failing test** (extend the class):
```cpp
void vapAccumulatesAndReaggregates();
...
void TestCryptoLadderModel::vapAccumulatesAndReaggregates() {
    CryptoLadderModel m(0.1);
    m.accumulate_vap(62601.0, 1.5);  // fine bucket 62601.0
    m.accumulate_vap(62603.0, 2.0);  // fine bucket 62603.0
    m.accumulate_vap(62601.0, 0.5);  // same fine bucket -> 2.0 total
    QVector<QPair<double,double>> bids{{62599,1.0}}, asks{{62601,1.0}};
    auto v = m.build(bids, asks, 10.0, 1); // grouping 10 -> both fine buckets fold into 62600
    auto row600 = std::find_if(v.rows.begin(), v.rows.end(),
                               [](const LadderRow& r){ return qAbs(r.price-62600.0)<1e-6; });
    QVERIFY(row600 != v.rows.end());
    QCOMPARE(row600->vap, 4.0);        // 2.0 + 2.0 re-aggregated into the 62600 bucket
    QCOMPARE(v.max_vap, 4.0);
    m.reset_vap();
    v = m.build(bids, asks, 10.0, 1);
    row600 = std::find_if(v.rows.begin(), v.rows.end(),
                          [](const LadderRow& r){ return qAbs(r.price-62600.0)<1e-6; });
    QCOMPARE(row600->vap, 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails** — build+run; Expected: FAIL (accumulate_vap undefined / vap always 0).

- [ ] **Step 3: Implement** — add to `CryptoLadderModel`:
```cpp
  private:
    double vap_base_ = 0.1;
    QHash<double,double> vap_;               // fine-bucket price -> summed volume
  public:
    explicit CryptoLadderModel(double vap_base = 0.1) : vap_base_(vap_base) {}
    void accumulate_vap(double price, double amount) {
        if (amount <= 0) return;
        vap_[bucket_price(price, vap_base_)] += amount;
    }
    void reset_vap() { vap_.clear(); }
```
In `build`, after computing each row's price, sum the fine buckets that fall in `[r.price, r.price + grouping)`:
```cpp
        for (auto it = vap_.begin(); it != vap_.end(); ++it) {
            const double rp = bucket_price(it.key(), grouping);
            // add to the matching display row if present (rows are contiguous)
        }
```
Concretely: build a `QHash<double,double> vap_by_display` by folding `vap_` through `bucket_price(key, grouping)`, then set `r.vap = vap_by_display.value(r.price, 0.0)` and update `v.max_vap`.

- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS (Task-1 tests still green).

- [ ] **Step 5: Neuter-verify** — key `accumulate_vap` at the display grouping (e.g. hardcode `bucket_price(price, 10.0)`) instead of `vap_base_`; confirm `vapAccumulatesAndReaggregates` still passes at grouping 10 BUT add an assertion at grouping 5 that now fails — simplest: temporarily make `reset_vap` a no-op and confirm the post-reset `QCOMPARE(row600->vap, 0.0)` FAILS; restore + `touch`.

- [ ] **Step 6: Commit**
```bash
git add src/screens/crypto_trading/CryptoLadderModel.h tests/tst_crypto_ladder_model.cpp
git commit -m "feat(crypto): ladder session VAP accumulator with grouping re-aggregation"
```

---

### Task 3: Orders / average-entry overlay in `build`

**Files:**
- Modify: `src/screens/crypto_trading/CryptoLadderModel.h`
- Test: `tests/tst_crypto_ladder_model.cpp` (extend)

**Interfaces:**
- Consumes: Task 1/2 `build`, `MyOrder`, `LadderRow`.
- Produces: `build` overload with overlays:
```cpp
LadderView build(const QVector<QPair<double,double>>& bids,
                 const QVector<QPair<double,double>>& asks,
                 double grouping, int rows_each_side,
                 const QVector<MyOrder>& my_orders,
                 double avg_entry_price) const;
```
The Task-1 4-arg `build` stays (delegates with `{}` and `0`). A `MyOrder` folds into its bucket's `my_bid_qty` (is_buy) / `my_ask_qty`; the row containing `avg_entry_price` gets `is_avg_entry = true`.

- [ ] **Step 1: Write the failing test**:
```cpp
void overlaysOrdersAndAvgEntry();
...
void TestCryptoLadderModel::overlaysOrdersAndAvgEntry() {
    CryptoLadderModel m;
    QVector<QPair<double,double>> bids{{62599,1.0}}, asks{{62601,1.0}};
    QVector<MyOrder> orders{{62589.0, 0.5, true},   // buy -> 62580 bucket
                            {62622.0, 0.3, false}};  // sell -> 62620 bucket
    auto v = m.build(bids, asks, 10.0, 3, orders, 62595.0); // avg entry -> 62590 bucket
    auto at = [&](double p){ return std::find_if(v.rows.begin(), v.rows.end(),
                             [p](const LadderRow& r){ return qAbs(r.price-p)<1e-6; }); };
    QCOMPARE(at(62580.0)->my_bid_qty, 0.5);
    QCOMPARE(at(62620.0)->my_ask_qty, 0.3);
    QVERIFY(at(62590.0)->is_avg_entry);
    QVERIFY(!at(62600.0)->is_avg_entry);
}
```

- [ ] **Step 2: Run test to verify it fails** — build+run; Expected: FAIL (6-arg build undefined).

- [ ] **Step 3: Implement** — add the 6-arg `build` (move the Task-1 body into it, keep the 4-arg as `return build(bids, asks, grouping, rows_each_side, {}, 0.0);`). After the rows are built:
```cpp
        QHash<double,double> mybuy, mysell;
        for (const auto& o : my_orders) {
            const double bp = bucket_price(o.price, grouping);
            (o.is_buy ? mybuy : mysell)[bp] += o.qty;
        }
        const double avg_bucket = avg_entry_price > 0 ? bucket_price(avg_entry_price, grouping) : -1;
        for (auto& r : v.rows) {
            r.my_bid_qty = mybuy.value(r.price, 0.0);
            r.my_ask_qty = mysell.value(r.price, 0.0);
            if (avg_bucket >= 0 && qAbs(r.price - avg_bucket) < 1e-6) r.is_avg_entry = true;
        }
```

- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS (all prior green).

- [ ] **Step 5: Neuter-verify** — swap `mybuy`/`mysell` (put buys into `my_ask_qty`); confirm `overlaysOrdersAndAvgEntry` FAILS; restore + `touch`.

- [ ] **Step 6: Commit**
```bash
git add src/screens/crypto_trading/CryptoLadderModel.h tests/tst_crypto_ladder_model.cpp
git commit -m "feat(crypto): ladder orders + avg-entry overlay in build"
```

---

### Task 4: `CryptoLadder` widget (paint + grouping + recenter)

**Files:**
- Create: `src/screens/crypto_trading/CryptoLadder.{h,cpp}`
- Modify: root `CMakeLists.txt` (add `CryptoLadder.cpp` to the GUI source list — grep how `CryptoOrderBook.cpp` is listed and add next to it)

**Interfaces:**
- Consumes: `CryptoLadderModel`, `LadderView`.
- Produces (the setters the screen feeds — Task 5 calls these):
```cpp
class CryptoLadder : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoLadder(QWidget* parent = nullptr);
    void set_book(const QVector<QPair<double,double>>& bids,
                  const QVector<QPair<double,double>>& asks); // depth update
    void add_trade(double price, double amount);              // one trade -> VAP
    void set_symbol(const QString& symbol, const QString& exchange); // resets VAP on change
    void set_my_orders(const QVector<crypto::MyOrder>& orders);
    void set_avg_entry(double price);
  protected:
    void paintEvent(QPaintEvent*) override;
    // no mousePressEvent that places/cancels orders — READ-ONLY (Phase 1)
};
```

- [ ] **Step 1** — Create `CryptoLadder.h/.cpp`. Structure mirrors `CryptoOrderBook` (custom paint, timer-throttled `update()`, theme-change repaint). Members: `crypto::CryptoLadderModel model_;` last book (`bids_`/`asks_`), `double grouping_`, `int rows_each_side_`, `QVector<crypto::MyOrder> my_orders_`, `double avg_entry_ = 0`, `QComboBox* grouping_cb_`, `bool attached_ = true` (recenter state), current `symbol_`/`exchange_`. Setters store data + `update()`. `add_trade` calls `model_.accumulate_vap(price, amount)`. `set_symbol` resets VAP + clears book when symbol/exchange changes. Grouping dropdown options: `{0.1, 1, 5, 10, 25, 100}` (all multiples of the model's 0.1 base); on change store `grouping_` + `update()`; default grouping chosen from mid magnitude (e.g. mid ≥ 10000 → 10). `paintEvent`: `const auto v = model_.build(bids_, asks_, grouping_, rows_each_side_, my_orders_, avg_entry_);` then draw the columns `ORDERS | BID | PRICE | ASK | VAP`, bids green below mid, asks red above, depth bars scaled to `v.max_depth`, VAP bar scaled to `v.max_vap`, `my_bid_qty`/`my_ask_qty` in the ORDERS column, and a highlight on the `is_avg_entry` row. A "recenter" affordance shows when `!attached_`.

- [ ] **Step 2: Build the GUI** — add to CMake, then `cmake --build /tmp/ot-build-test --target OpenMarketTerminal` (or the GUI target). Expected: rc 0. Confirm no order-place/cancel calls in the file: `grep -niE "place_order|cancel|submit|UnifiedTrading" src/screens/crypto_trading/CryptoLadder.cpp` → must be empty.

- [ ] **Step 3: Manual verification note** — no unit test (paint widget, per `CryptoOrderBook` precedent). In your report, confirm rc 0, paste the empty grep result (proving read-only), and describe the paint layout.

- [ ] **Step 4: Commit**
```bash
git add src/screens/crypto_trading/CryptoLadder.h src/screens/crypto_trading/CryptoLadder.cpp CMakeLists.txt
git commit -m "feat(crypto): CryptoLadder DOM widget — paint + grouping + recenter (read-only)"
```

---

### Task 5: Wire the ladder into the crypto trading screen

**Files:**
- Modify: `src/screens/crypto_trading/CryptoTradingScreen.{h,cpp}` (own a `CryptoLadder* ladder_`, create it, subscribe-feed it), `CryptoTradingScreen_Refresh.cpp` (feed depth + trades where `orderbook_->set_data` / `bottom_panel_->set_trades` are called), and the layout registration.

**Interfaces:**
- Consumes: `CryptoLadder` setters (Task 4).

- [ ] **Step 1** — In `CryptoTradingScreen`, add `CryptoLadder* ladder_ = nullptr;` and create it next to where `orderbook_` is created; add it to the layout as a dockable panel following exactly how `orderbook_` / `CryptoDepthChart` are placed (read `CryptoTradingScreen.cpp` around the widget construction + `LayoutCatalog`/dock registration and mirror it). Read `CryptoTradingScreen.cpp:537-562` (the `hub.subscribe<OrderBookData>` handler) and `CryptoTradingScreen_Refresh.cpp:97,272` (`orderbook_->set_data(ob.bids, ob.asks, ...)`): at each of those sites ALSO call `ladder_->set_book(ob.bids, ob.asks);` and `ladder_->set_symbol(current_symbol, current_exchange);`. Where trades arrive (`bottom_panel_->set_trades(...)` at Refresh.cpp:198,321 and the CryptoTimeSales `add_trade` path), also feed the ladder: for a batch, iterate and `ladder_->add_trade(t.price, t.amount)`; for the live single-trade path, `ladder_->add_trade(...)` per trade. If a live orders/position feed is readily available where the account panel is updated, call `ladder_->set_my_orders(...)` + `ladder_->set_avg_entry(...)`; if that feed isn't cleanly reachable in Phase 1, leave the overlay empty and NOTE it (market depth + VAP still render) — do not invent a new subscription.

- [ ] **Step 2: Build the GUI** — `cmake --build /tmp/ot-build-test --target OpenMarketTerminal` (rc 0) and the in-tree `cmake --build openmarketterminal-qt/build` (rc 0).

- [ ] **Step 3: Full ctest** — `cd /tmp/ot-build-test && ctest` — report N/N (must stay all-green; the model tests + existing suite).

- [ ] **Step 4: Manual verification** — launch the GUI, open the crypto trading screen for BTC/USD, confirm the ladder panel renders live depth + VAP heatmap, the grouping dropdown re-buckets, and (if wired) your orders/avg-entry show. Document the result and the READ-ONLY grep for the wiring (no order-place calls added).

- [ ] **Step 5: Commit**
```bash
git add src/screens/crypto_trading/CryptoTradingScreen.h src/screens/crypto_trading/CryptoTradingScreen.cpp src/screens/crypto_trading/CryptoTradingScreen_Refresh.cpp
git commit -m "feat(crypto): wire DOM ladder panel into the crypto trading screen"
```

---

## Self-Review notes

- **Spec coverage:** grouping dropdown (T4), bucketing (T1), session VAP + re-aggregation at fine base (T2), orders/avg-entry overlay (T3), paint columns ORDERS|BID|PRICE|ASK|VAP + heatmap + recenter (T4), panel placement via existing layout (T5), read-only / no order-mutation (Global + T4/T5 grep gates), pure-model unit tests (T1–T3), widget manual-verified (T4–T5). All mapped.
- **Type consistency:** `LadderRow`/`LadderView`/`MyOrder`/`CryptoLadderModel::build` defined in T1 and reused verbatim in T2/T3/T4; the 4-arg `build` (T1) delegates to the 6-arg `build` (T3); widget setters (T4) are called by T5.
- **YAGNI / safety:** no click-to-trade, no `UnifiedTrading`, no order structs from the trading layer leak into the model (`MyOrder` is local). Phase-2 click-to-trade is a separate spec.
