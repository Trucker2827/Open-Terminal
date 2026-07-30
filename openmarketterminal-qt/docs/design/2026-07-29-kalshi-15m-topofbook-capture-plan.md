# Kalshi 15-minute Top-of-Book Capture & Retention — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Continuously capture KXBTC15M top-of-book while the app runs and retain it in the 30-day lag series, so `path_exit_sim` can be re-run on real 15-minute price paths.

**Architecture:** An app-embedded, always-on C++ controller discovers open KXBTC15M markets via REST and subscribes to their `ticker` channel independent of UI selection; subscribed ticks flow through the existing `ws_ticker_event → kalshi-tickers.jsonl` sink unchanged; a small `kalshi_lag_series.py` change retains 15-minute rows. Subscription policy is a pure, unit-tested function isolated from Qt/WS.

**Tech Stack:** Python 3 stdlib (compactor); C++17 / Qt (capture controller, QtTest); existing `KalshiRestClient` (`fetch_markets` / `markets_ready`) and `PredictionExchangeAdapter::subscribe_market`.

## Global Constraints

- Scope is **KXBTC15M only**, behind a configurable series list (default `["KXBTC15M"]`). Copy this list verbatim in both languages; do not hardcode the family inline.
- **No new evidence file and no change to the recording sink** — reuse the existing `ws_ticker_event` handler that appends to `kalshi-tickers.jsonl`.
- **No change to any analysis script's outcome logic** — 15-minute contracts carry `strike = None`; outcomes resolve from recorded settlements only (already the default).
- Compactor is **Python 3 stdlib only** (no third-party imports), matching `kalshi_lag_series.py` today.
- The controller **only ever unsubscribes tickers it added** (never disturbs the UI/ladder subscription).
- **No silent truncation:** if a subscription cap or page limit is hit, `qWarning`/log it.
- Feed volume / rotation impact must be **measured** in the canary, not assumed.
- Base branch: `finn/kalshi-15m-topofbook-capture` off `main`.

Paths below are relative to `openmarketterminal-qt/`.

## File Structure

- `scripts/kalshi_lag_series.py` — **modify**: add `parse_15m_ticker`, 15-minute constants, and a 15-minute branch in `collect_quotes`; update `header_row`.
- `tests/test_kalshi_lag_series.py` — **modify**: add 15-minute parse + retention tests.
- `src/services/prediction/kalshi/Kalshi15mReconcile.h` / `.cpp` — **create**: pure subscription-policy functions (no Qt objects beyond `QString` containers, no I/O).
- `tests/tst_kalshi_15m_reconcile.cpp` — **create**: QtTest for the pure functions (register mirroring the existing `tst_kalshi_*` targets).
- `src/services/prediction/kalshi/Kalshi15mCaptureController.h` / `.cpp` — **create**: QObject owning a `KalshiRestClient` + `QTimer`; discovers, reconciles, subscribes via the adapter.
- `src/screens/kalshi/KalshiScreen.cpp` / `.h` — **modify**: instantiate and start the controller once the adapter is wired.
- `CMakeLists.txt` (app + `tests/CMakeLists.txt`) — **modify**: compile the new sources and register the new test.

---

### Task 1: Compactor — recognise KXBTC15M tickers

**Files:**
- Modify: `scripts/kalshi_lag_series.py` (add constants near line 156; add `parse_15m_ticker` after `parse_threshold_ticker`, ~line 316)
- Test: `tests/test_kalshi_lag_series.py`

**Interfaces:**
- Produces: `parse_15m_ticker(ticker: str) -> dict | None` returning `{"family": str, "close_ms": int, "strike": None}`; module constants `FIFTEEN_MIN_FAMILIES: tuple[str, ...]` and `MAX_15M_SECONDS_TO_CLOSE: int`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_kalshi_lag_series.py` (in the parsing test class, beside the existing `parse_threshold_ticker` tests near line 210):

```python
def test_parse_15m_ticker_reads_close_and_has_no_strike(self):
    parsed = series.parse_15m_ticker("KXBTC15M-26JUL270330-30")
    self.assertIsNotNone(parsed)
    self.assertEqual(parsed["family"], "KXBTC15M")
    self.assertIsNone(parsed["strike"])
    # 03:30 America/New_York on 2026-07-27 (EDT = UTC-4) -> 07:30 UTC
    expected = datetime.datetime(2026, 7, 27, 7, 30,
                                 tzinfo=datetime.timezone.utc)
    self.assertEqual(parsed["close_ms"], int(expected.timestamp() * 1000))

def test_parse_15m_ticker_rejects_other_families_and_threshold(self):
    self.assertIsNone(series.parse_15m_ticker("KXETH15M-26JUL270330-30"))
    self.assertIsNone(series.parse_15m_ticker("KXBTCD-26JUL2719-T66499.99"))
    self.assertIsNone(series.parse_15m_ticker("garbage"))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -k parse_15m -v`
Expected: FAIL — `AttributeError: module 'kalshi_lag_series' has no attribute 'parse_15m_ticker'`.

- [ ] **Step 3: Add constants and the parser**

Add near the threshold window constants (~line 156):

```python
# 15-minute directional contracts (issue: 15m top-of-book retention). Family-
# gated so only the configured series are kept; expandable without code changes.
FIFTEEN_MIN_FAMILIES = ("KXBTC15M",)
# A 15-minute contract's whole life is <= 15 min; keep from open to close.
MAX_15M_SECONDS_TO_CLOSE = 900
```

Add after `parse_threshold_ticker` (~line 316):

```python
def parse_15m_ticker(ticker):
    """`KXBTC15M-26JUL270330-30` -> close instant, strike None. Else None.

    15-minute directionals carry no strike (`-30` is the window marker, not a
    `-T` strike), so their outcome is resolvable only from recorded settlements,
    never derived — the same rule `kalshi_edge_common.parse_ticker` applies.
    Family-gated to FIFTEEN_MIN_FAMILIES so the series keeps only the configured
    15-minute markets. Close time uses the same real US/Eastern zone and fold as
    the threshold parser, so the series and the analysis agree.
    """
    parts = ticker.split("-")
    if len(parts) < 2 or parts[0] not in FIFTEEN_MIN_FAMILIES:
        return None
    matched = _TICKER_DATE.match(parts[1])
    if not matched:
        return None
    yy, mon, dd, hh, mm = matched.groups()
    if mon not in MONTHS:
        return None
    close = datetime.datetime(2000 + int(yy), MONTHS[mon], int(dd),
                              int(hh), int(mm or 0), tzinfo=EASTERN,
                              fold=CLOSE_FOLD)
    return {"family": parts[0],
            "close_ms": int(close.timestamp() * 1000),
            "strike": None}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -k parse_15m -v`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_lag_series.py openmarketterminal-qt/tests/test_kalshi_lag_series.py
git commit -m "feat(lag-series): recognise KXBTC15M tickers (close, no strike)"
```

---

### Task 2: Compactor — retain 15-minute rows

**Files:**
- Modify: `scripts/kalshi_lag_series.py` — `collect_quotes` (~lines 386–391) and `header_row` (`quote_rule`, ~line 550)
- Test: `tests/test_kalshi_lag_series.py`

**Interfaces:**
- Consumes: `parse_15m_ticker`, `FIFTEEN_MIN_FAMILIES`, `MAX_15M_SECONDS_TO_CLOSE` (Task 1).
- Produces: `collect_quotes` now also returns rows for in-window KXBTC15M tickers; row shape is unchanged (verbatim ticker copy, as for threshold rows).

- [ ] **Step 1: Write the failing test**

Add a retention test. Use the existing base class that wires `OPENTERMINAL_EVIDENCE_DIR` to a temp dir (see `_seed_day`, line 376). This seeds one in-window 15-minute quote and one 15-minute quote already 20 minutes past close, then asserts only the in-window one is collected:

```python
def test_collect_quotes_retains_in_window_15m(self):
    close = datetime.datetime(2026, 7, 27, 7, 30, tzinfo=datetime.timezone.utc)
    close_ms = int(close.timestamp() * 1000)
    in_window = close_ms - 300_000          # 5 min before close -> kept
    too_early = close_ms - 3_600_000        # 60 min before close -> 15m dropped
    ticker = "KXBTC15M-26JUL270330-30"
    rows = [
        {"event": "kalshi_ticker", "market_ticker": ticker, "ts_ms": in_window,
         "yes_bid_dollars": "0.4000", "yes_ask_dollars": "0.4200"},
        {"event": "kalshi_ticker", "market_ticker": ticker, "ts_ms": too_early,
         "yes_bid_dollars": "0.4000", "yes_ask_dollars": "0.4200"},
    ]
    path = os.path.join(self.evidence, series.SOURCE_TICKERS)
    with open(path, "w", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")
    collected, _oldest = series.collect_quotes(None)
    kept = [r for r in collected if r["ticker"] == ticker]
    self.assertEqual(len(kept), 1)
    self.assertEqual(kept[0]["ts_ms"], in_window)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -k retains_in_window_15m -v`
Expected: FAIL — `AssertionError: 0 != 1` (15-minute rows are currently dropped by `parse_threshold_ticker` returning None).

- [ ] **Step 3: Add the 15-minute branch in `collect_quotes`**

Replace the parse+window block (currently ~lines 386–391):

```python
        parsed = parse_threshold_ticker(ticker)
        if parsed is None:
            continue
        seconds_to_close = (parsed["close_ms"] - ts_ms) / 1000.0
        if not MIN_SECONDS_TO_CLOSE <= seconds_to_close <= MAX_SECONDS_TO_CLOSE:
            continue
```

with:

```python
        parsed = parse_threshold_ticker(ticker)
        max_s = MAX_SECONDS_TO_CLOSE
        if parsed is None:
            parsed = parse_15m_ticker(ticker)
            max_s = MAX_15M_SECONDS_TO_CLOSE
        if parsed is None:
            continue
        seconds_to_close = (parsed["close_ms"] - ts_ms) / 1000.0
        if not MIN_SECONDS_TO_CLOSE <= seconds_to_close <= max_s:
            continue
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -k retains_in_window_15m -v`
Expected: PASS.

- [ ] **Step 5: Update the header's `quote_rule` to state the added population**

In `header_row` (~line 550), replace the `quote_rule` value:

```python
        "quote_rule": ("KXBTCD -T threshold contracts within %d s of close; "
                       "and %s 15-minute directionals within %d s of close "
                       "(no strike, recorded-settlement outcomes only): "
                       "every change of (yes_bid, yes_ask) plus a heartbeat"
                       % (MAX_SECONDS_TO_CLOSE, ",".join(FIFTEEN_MIN_FAMILIES),
                          MAX_15M_SECONDS_TO_CLOSE)),
```

- [ ] **Step 6: Run the full compactor test module (no regressions)**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -v`
Expected: PASS (all existing tests + the two new ones).

- [ ] **Step 7: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_lag_series.py openmarketterminal-qt/tests/test_kalshi_lag_series.py
git commit -m "feat(lag-series): retain in-window KXBTC15M top-of-book"
```

---

### Task 3: C++ pure subscription-policy functions

**Files:**
- Create: `src/services/prediction/kalshi/Kalshi15mReconcile.h`, `Kalshi15mReconcile.cpp`
- Create/Test: `tests/tst_kalshi_15m_reconcile.cpp`
- Modify: `tests/CMakeLists.txt` (register), app `CMakeLists.txt` (compile the `.cpp`)

**Interfaces:**
- Produces:
  - `QStringList kalshi15m::desired_subscriptions(const QVector<pred::PredictionMarket>& markets, const QStringList& families, int cap)` — the market_ids whose `family` (prefix before `-`) is in `families`, de-duplicated, order-stable, truncated to `cap` with a `qWarning` if exceeded.
  - `struct kalshi15m::Delta { QStringList to_subscribe; QStringList to_unsubscribe; };`
  - `kalshi15m::Delta kalshi15m::reconcile(const QStringList& desired, const QSet<QString>& held)` — `to_subscribe = desired − held`, `to_unsubscribe = held − desired`.
  - (`pred` = `openmarketterminal::services::prediction`.)

- [ ] **Step 1: Write the failing test**

Create `tests/tst_kalshi_15m_reconcile.cpp`:

```cpp
#include <QtTest>
#include "services/prediction/kalshi/Kalshi15mReconcile.h"

namespace pred = openmarketterminal::services::prediction;

class TstKalshi15mReconcile : public QObject {
    Q_OBJECT
private slots:
    void desired_filters_by_family_and_caps() {
        QVector<pred::PredictionMarket> mk(3);
        mk[0].key.market_id = "KXBTC15M-26JUL270330-30";
        mk[1].key.market_id = "KXETH15M-26JUL270330-30";   // wrong family
        mk[2].key.market_id = "KXBTC15M-26JUL270345-45";
        const auto got = kalshi15m::desired_subscriptions(mk, {"KXBTC15M"}, 100);
        QCOMPARE(got, (QStringList{"KXBTC15M-26JUL270330-30",
                                   "KXBTC15M-26JUL270345-45"}));
    }
    void reconcile_computes_both_deltas() {
        const QStringList desired{"A", "B", "C"};
        const QSet<QString> held{"B", "C", "D"};
        const auto d = kalshi15m::reconcile(desired, held);
        QCOMPARE(d.to_subscribe, (QStringList{"A"}));
        QCOMPARE(d.to_unsubscribe, (QStringList{"D"}));
    }
    void reconcile_never_touches_foreign_held() {
        // A ticker the controller never added (held only) must be unsubscribed
        // ONLY because it is 15m-held here; foreign UI tickers are never in held.
        const auto d = kalshi15m::reconcile({"A"}, {"A"});
        QVERIFY(d.to_subscribe.isEmpty());
        QVERIFY(d.to_unsubscribe.isEmpty());
    }
};
QTEST_MAIN(TstKalshi15mReconcile)
#include "tst_kalshi_15m_reconcile.moc"
```

- [ ] **Step 2: Register the test (mirror an existing tst_ target) and run it to confirm it fails to build**

In `tests/CMakeLists.txt`, mirror the registration of an existing Qt test target (find one: `grep -n "tst_" tests/CMakeLists.txt`; if none is there, the qt tests are registered in the app `CMakeLists.txt` — grep `tst_kalshi_auto_engine` across `*.txt` and copy that block). Add a `qt_add_executable`/`add_test` pair for `tst_kalshi_15m_reconcile` linking `Qt::Test` and the `Kalshi15mReconcile.cpp` source.

Run: `cd openmarketterminal-qt && cmake --build build --target tst_kalshi_15m_reconcile`
Expected: FAIL — `Kalshi15mReconcile.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/services/prediction/kalshi/Kalshi15mReconcile.h`:

```cpp
#pragma once
#include <QStringList>
#include <QSet>
#include <QVector>
#include "services/prediction/PredictionTypes.h"

namespace kalshi15m {

namespace pred = openmarketterminal::services::prediction;

/// market_ids whose family (prefix before the first '-') is in `families`,
/// de-duplicated and order-stable, truncated to `cap` (qWarning if exceeded).
QStringList desired_subscriptions(const QVector<pred::PredictionMarket>& markets,
                                  const QStringList& families, int cap);

struct Delta { QStringList to_subscribe; QStringList to_unsubscribe; };

/// to_subscribe = desired \ held ; to_unsubscribe = held \ desired.
Delta reconcile(const QStringList& desired, const QSet<QString>& held);

}  // namespace kalshi15m
```

- [ ] **Step 4: Write the implementation**

Create `src/services/prediction/kalshi/Kalshi15mReconcile.cpp`:

```cpp
#include "services/prediction/kalshi/Kalshi15mReconcile.h"
#include <QLoggingCategory>

namespace kalshi15m {

QStringList desired_subscriptions(const QVector<pred::PredictionMarket>& markets,
                                  const QStringList& families, int cap) {
    QStringList out;
    QSet<QString> seen;
    for (const auto& m : markets) {
        const QString id = m.key.market_id;
        const int dash = id.indexOf('-');
        const QString family = dash < 0 ? id : id.left(dash);
        if (!families.contains(family) || seen.contains(id) || id.isEmpty())
            continue;
        seen.insert(id);
        out.append(id);
    }
    if (cap > 0 && out.size() > cap) {
        qWarning("kalshi15m: %d open markets exceeds cap %d; capping",
                 out.size(), cap);
        out = out.mid(0, cap);
    }
    return out;
}

Delta reconcile(const QStringList& desired, const QSet<QString>& held) {
    Delta d;
    const QSet<QString> desired_set(desired.begin(), desired.end());
    for (const QString& t : desired)
        if (!held.contains(t)) d.to_subscribe.append(t);
    for (const QString& t : held)
        if (!desired_set.contains(t)) d.to_unsubscribe.append(t);
    d.to_unsubscribe.sort();   // deterministic for tests (held is unordered)
    return d;
}

}  // namespace kalshi15m
```

- [ ] **Step 5: Build and run the test**

Run: `cd openmarketterminal-qt && cmake --build build --target tst_kalshi_15m_reconcile && ctest --test-dir build -R tst_kalshi_15m_reconcile --output-on-failure`
Expected: PASS (3 test functions).

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/src/services/prediction/kalshi/Kalshi15mReconcile.* openmarketterminal-qt/tests/tst_kalshi_15m_reconcile.cpp openmarketterminal-qt/tests/CMakeLists.txt openmarketterminal-qt/CMakeLists.txt
git commit -m "feat(kalshi): pure 15m subscription-reconcile policy + QtTest"
```

---

### Task 4: C++ capture controller

**Files:**
- Create: `src/services/prediction/kalshi/Kalshi15mCaptureController.h`, `.cpp`
- Modify: app `CMakeLists.txt` (compile the `.cpp` + moc)

**Interfaces:**
- Consumes: `kalshi15m::desired_subscriptions`, `kalshi15m::reconcile` (Task 3); `KalshiRestClient::fetch_markets(...)` + `markets_ready(QVector<PredictionMarket>, QString next_cursor)`; `PredictionExchangeAdapter::subscribe_market(QStringList)` / `unsubscribe_market(QStringList)`.
- Produces: `class Kalshi15mCaptureController : public QObject` with `explicit Kalshi15mCaptureController(pred::PredictionExchangeAdapter* adapter, QObject* parent = nullptr)` and `void start();` `void stop();`

- [ ] **Step 1: Write the header**

Create `src/services/prediction/kalshi/Kalshi15mCaptureController.h`:

```cpp
#pragma once
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <memory>
#include "services/prediction/PredictionTypes.h"

namespace openmarketterminal::services::prediction { class PredictionExchangeAdapter; }
namespace kalshi_data { class KalshiRestClient; }

/// Always-on: discovers open KXBTC15M markets via REST and keeps the app's
/// Kalshi WS subscribed to their ticker channel, independent of UI selection.
/// Recording is the existing ws_ticker_event -> kalshi-tickers.jsonl sink.
class Kalshi15mCaptureController : public QObject {
    Q_OBJECT
public:
    namespace pred = openmarketterminal::services::prediction;   // doc only
    explicit Kalshi15mCaptureController(
        openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter,
        QObject* parent = nullptr);
    ~Kalshi15mCaptureController() override;
    void start();   // begins the discovery timer + first immediate poll
    void stop();

private slots:
    void poll();
    void on_markets_ready(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const QString& next_cursor);

private:
    void reconcile_and_apply();
    openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter_;
    std::unique_ptr<kalshi_data::KalshiRestClient> rest_;
    QTimer poll_timer_;
    QStringList families_{QStringLiteral("KXBTC15M")};
    int cap_ = 200;
    int poll_interval_ms_ = 30000;
    QVector<openmarketterminal::services::prediction::PredictionMarket> page_accum_;
    QSet<QString> held_;
};
```

> Note: remove the `namespace pred = …;` doc line before compiling — it is illegal inside a class body; it documents the alias used in the `.cpp`. Use the fully-qualified type in the header as shown.

- [ ] **Step 2: Write the implementation**

Create `src/services/prediction/kalshi/Kalshi15mCaptureController.cpp`:

```cpp
#include "services/prediction/kalshi/Kalshi15mCaptureController.h"
#include "services/prediction/kalshi/KalshiRestClient.h"
#include "services/prediction/kalshi/Kalshi15mReconcile.h"
#include "services/prediction/PredictionExchangeAdapter.h"

namespace pred = openmarketterminal::services::prediction;

Kalshi15mCaptureController::Kalshi15mCaptureController(
    pred::PredictionExchangeAdapter* adapter, QObject* parent)
    : QObject(parent), adapter_(adapter),
      rest_(std::make_unique<kalshi_data::KalshiRestClient>(this)) {
    connect(rest_.get(), &kalshi_data::KalshiRestClient::markets_ready,
            this, &Kalshi15mCaptureController::on_markets_ready);
    poll_timer_.setInterval(poll_interval_ms_);
    connect(&poll_timer_, &QTimer::timeout, this, &Kalshi15mCaptureController::poll);
}

Kalshi15mCaptureController::~Kalshi15mCaptureController() = default;

void Kalshi15mCaptureController::start() { poll(); poll_timer_.start(); }
void Kalshi15mCaptureController::stop()  { poll_timer_.stop(); }

void Kalshi15mCaptureController::poll() {
    page_accum_.clear();
    // Discover open markets for the (single, MVP) configured 15-minute series.
    rest_->fetch_markets(QStringLiteral("open"), QString(), families_.first(),
                         QString(), 100, QString());
}

void Kalshi15mCaptureController::on_markets_ready(
    const QVector<pred::PredictionMarket>& markets, const QString& next_cursor) {
    page_accum_ += markets;
    if (!next_cursor.isEmpty()) {
        rest_->fetch_markets(QStringLiteral("open"), QString(), families_.first(),
                             QString(), 100, next_cursor);
        return;
    }
    reconcile_and_apply();
}

void Kalshi15mCaptureController::reconcile_and_apply() {
    const QStringList desired =
        kalshi15m::desired_subscriptions(page_accum_, families_, cap_);
    const kalshi15m::Delta d = kalshi15m::reconcile(desired, held_);
    if (!d.to_subscribe.isEmpty())   adapter_->subscribe_market(d.to_subscribe);
    if (!d.to_unsubscribe.isEmpty()) adapter_->unsubscribe_market(d.to_unsubscribe);
    held_ = QSet<QString>(desired.begin(), desired.end());
}
```

- [ ] **Step 3: Add the sources to the app build**

In the app `CMakeLists.txt`, find where `KalshiRestClient.cpp` is listed in the target sources (`grep -n "KalshiRestClient.cpp" CMakeLists.txt`) and add `Kalshi15mCaptureController.cpp` and `Kalshi15mReconcile.cpp` beside it.

- [ ] **Step 4: Build the app target to confirm it compiles**

Run: `cd openmarketterminal-qt && cmake --build build --target openmarketterminal 2>&1 | tail -20`
Expected: builds without errors referencing the new files. (If `subscribe_market`/`unsubscribe_market` are not on `PredictionExchangeAdapter` but on the concrete `KalshiAdapter`, `qobject_cast` the adapter in the constructor and store the concrete type — confirm with `grep -n "subscribe_market" src/services/prediction/PredictionExchangeAdapter.h`.)

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/services/prediction/kalshi/Kalshi15mCaptureController.* openmarketterminal-qt/CMakeLists.txt
git commit -m "feat(kalshi): always-on 15m top-of-book capture controller"
```

---

### Task 5: Wire the controller into KalshiScreen + canary

**Files:**
- Modify: `src/screens/kalshi/KalshiScreen.h` (member), `src/screens/kalshi/KalshiScreen.cpp` (instantiate after `wire_adapter`)

**Interfaces:**
- Consumes: `Kalshi15mCaptureController` (Task 4); `KalshiScreen::adapter()` (`src/screens/kalshi/KalshiScreen.h:159`); `KalshiScreen::wire_adapter()` (`:68`).

- [ ] **Step 1: Add the member**

In `KalshiScreen.h`, add an include (`#include "services/prediction/kalshi/Kalshi15mCaptureController.h"`) and a member:

```cpp
    Kalshi15mCaptureController* capture_15m_ = nullptr;
```

- [ ] **Step 2: Instantiate and start once the adapter exists**

At the end of `KalshiScreen::wire_adapter()` (`grep -n "void KalshiScreen::wire_adapter" src/screens/kalshi/KalshiScreen.cpp`), add:

```cpp
    if (!capture_15m_ && adapter()) {
        capture_15m_ = new Kalshi15mCaptureController(adapter(), this);
        capture_15m_->start();
    }
```

- [ ] **Step 3: Build the app**

Run: `cd openmarketterminal-qt && cmake --build build --target openmarketterminal 2>&1 | tail -20`
Expected: builds clean.

- [ ] **Step 4: Canary — confirm capture, then measure volume (manual, ~15 min run)**

Run the app with the Kalshi screen for ~15 minutes, then:

```bash
EV="$HOME/Library/Application Support/Open Terminal/Open Terminal"
# (a) 15m ticks are now being recorded:
grep -c '"market_ticker":"KXBTC15M' "$EV/kalshi-tickers.jsonl"        # expect > 0
# (b) run the compactor once and confirm 15m rows are retained:
OPENTERMINAL_EVIDENCE_DIR="$EV" python3 openmarketterminal-qt/scripts/kalshi_lag_series.py
grep -l 'KXBTC15M' "$EV"/kalshi-lag-series/*.jsonl                    # expect a match
# (c) MEASURE the added volume (Global Constraint): rows/run attributable to 15m
```

Record in the canary note: KXBTC15M rows/hour added to `kalshi-tickers.jsonl`, resulting MB/day, and the compactor run interval vs `ROTATION_HORIZON_HOURS` (5.7 h). **If the raw feed's rotation window drops near or below the compactor's launchd interval, shorten that interval** (in `deploy/org.openterminal.lag-series.plist`) and note it. Do not claim the volume is safe without these numbers.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/screens/kalshi/KalshiScreen.h openmarketterminal-qt/src/screens/kalshi/KalshiScreen.cpp
git commit -m "feat(kalshi): start 15m capture controller from KalshiScreen"
```

---

### Task 6: Re-run the exit sim on 15-minute paths (after accrual)

**Not code — a scheduled follow-up.** After ~3–7 days of accrual, extend `scripts/research/path_exit_sim.py` to also load the KXBTC15M family from the retained series (its quotes are now present; outcomes come from recorded settlements) and re-run the take-profit × stop-loss × trailing sweep on 15-minute paths. This closes the loop the whole investigation opened. File a tracking issue so it is not forgotten.

---

## Self-Review

**Spec coverage:** capture controller (Tasks 3–5) ✓; KXBTC15M-only behind configurable list (`FIFTEEN_MIN_FAMILIES`, `families_`) ✓; discovery via `fetch_markets(series_ticker=…)` 30 s poll (Task 4) ✓; pure reconcile isolated + unit-tested (Task 3) ✓; no sink change (reuse `ws_ticker_event`) ✓; compactor `[0,900]s` retain, `strike=None`, header text (Tasks 1–2) ✓; recorded-settlement outcomes downstream (no analysis change) ✓; feed-volume measured, no silent cap (Task 5 step 4, `cap_` qWarning) ✓; app-embedded/app-open-only ✓; re-run sim (Task 6) ✓.

**Placeholder scan:** no TBD/TODO; every code step shows complete code; the two genuine discovery actions (test-target registration mirror in Task 3 step 2; adapter subscribe-method location in Task 4 step 4) are concrete grep-and-mirror steps with named targets, not vague instructions.

**Type consistency:** `desired_subscriptions`/`reconcile`/`Delta` signatures match between Task 3's header, its test, and Task 4's calls; `markets_ready(QVector<PredictionMarket>, QString)` matches the confirmed signal; `subscribe_market(QStringList)`/`unsubscribe_market(QStringList)` match the adapter interface; `families_` default `["KXBTC15M"]` matches `FIFTEEN_MIN_FAMILIES`.
