# Kalshi paper bot — session-budget deadlock + orphan leak — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the paper bot bricking itself: exempt the perpetual paper loop from the live bounded-run session budget, and retire (void) provably-orphaned paper positions so open exposure stops leaking.

**Architecture:** Two independent defects in the paper decision engine. Fix 1 adds a `Config` flag so `decide()` skips the cumulative `session_budget_usd` check in the paper loop (`run_tick`), leaving live's bounded-run safety untouched. Fix 2 has `settle_paper` emit a distinct `kalshi_bot_paper_void` event for positions older than a horizon that exceeds any contract lifetime + settlement retention, and `KalshiBotOrders::replay` retires the position on that event — never fabricating a win/loss and never counted by the gate/funnel (which filter strictly on `kalshi_bot_paper_settlement`).

**Tech Stack:** C++20, Qt6 (QJsonObject/Array), QtTest.

## Global Constraints

- **Never fabricate a settlement outcome.** A void row carries `won: null`, `realized_pnl: 0.0`, `resolution: "unresolved_expired"`; it is NOT a win, loss, or settled bet.
- **No look-ahead.** Void eligibility is purely `now_ms - placed_ms >= kOrphanVoidAgeMs` (position age), never a peek at later market state, never a ticker-parsed (ET-encoded) close time.
- **Live-mode session budget is unchanged.** `Config::enforce_session_budget` defaults `true`; only the paper loop (`run_tick`) sets it `false`. `run_live_tick` is not touched.
- **Reuse existing constants.** Event names via `kSettlementEvent`/`kVoidEvent`/`kDecisionEvent` constants (duplicated per-file, matching the existing `kSettlementEvent` pattern); reason codes via `KalshiBotDecision::k*` constants.
- Exact anchors: `KalshiBotDecision.cpp` — `decide()` session-budget check at `:399-402`, `settle_paper` at `:494-542`, stay-open branch at `:510-512`. `KalshiBotOrders.cpp` — settlement retirement at `:136-139`. `KalshiBotCommands.cpp` — `run_tick` at `:308`, its `decide()` call at `:428`, stopped-path `decide()` at `:328`, `reconcile()` at `:403`. Config at `KalshiBotDecision.h:148-201` (`session_budget_usd` at the line reading `double session_budget_usd = 120.00;`).

---

### Task 1: Exempt the paper loop from the lifetime session budget (Fix 1)

**Files:**
- Modify: `openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotDecision.h` (Config struct)
- Modify: `openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotDecision.cpp` (`decide()` session-budget guard)
- Modify: `openmarketterminal-qt/src/cli/KalshiBotCommands.cpp` (`run_tick` uses a paper config)
- Test: `openmarketterminal-qt/tests/tst_kalshi_bot_decision.cpp`

**Interfaces:**
- Produces: `KalshiBotDecision::Config::enforce_session_budget` (bool, default `true`). When `false`, `decide()` does not enforce `session_budget_usd`; `max_open_exposure_usd` is still enforced.

- [ ] **Step 1: Write the failing tests**

Add to `tst_kalshi_bot_decision.cpp` (uses existing `report`, `action`, `reason`, `only_row` helpers). The bidding report `report(0.98, 0.83, 10.0)` has edge `0.15 > 0.10` and is the file's existing positive control.

```cpp
// --- Fix 1: the paper loop is not bricked by the lifetime session budget ---

/// With the budget exempted (paper), a bid still fires even though the
/// session's cumulative all-in is already at the cap. Neuter below proves the
/// flag is what gates it and live is unchanged.
void paper_accumulates_past_the_session_budget() {
    const QJsonObject bidding = report(0.98, 0.83, 10.0);

    KalshiBotDecision::Exposure at_cap;                 // cumulative opens at the cap
    at_cap.session_opened_usd = 120.00;

    KalshiBotDecision::Config paper;                    // paper: budget exempt
    paper.enforce_session_budget = false;
    const QJsonArray bids = KalshiBotDecision::decide(bidding, {}, {}, kNow, paper, {}, at_cap);
    QCOMPARE(action(bids), QStringLiteral("bid"));

    // Neuter: default config (live semantics) refuses the very same inputs.
    const QJsonArray blocked = KalshiBotDecision::decide(bidding, {}, {}, kNow, {}, {}, at_cap);
    QCOMPARE(action(blocked), QStringLiteral("pass"));
    QCOMPARE(reason(blocked), QStringLiteral("SESSION_BUDGET_BLOCKS_BID"));
}

/// Exempting the session budget must NOT make paper unbounded: the current
/// open-exposure fence still refuses a bid that would breach it.
void paper_still_respects_the_open_exposure_cap() {
    const QJsonObject bidding = report(0.98, 0.83, 10.0);

    KalshiBotDecision::Exposure at_open_cap;            // open risk already at the cap
    at_open_cap.at_risk_usd = 120.00;

    KalshiBotDecision::Config paper;
    paper.enforce_session_budget = false;
    const QJsonArray rows = KalshiBotDecision::decide(bidding, {}, {}, kNow, paper, {}, at_open_cap);
    QCOMPARE(action(rows), QStringLiteral("pass"));
    QCOMPARE(reason(rows), QStringLiteral("EXPOSURE_CAP_BLOCKS_BID"));
}
```

- [ ] **Step 2: Run the tests to confirm they fail**

Build the test target and run. `paper_accumulates_past_the_session_budget` fails to compile (no `enforce_session_budget` member) — that IS the red state; fix compilation via Step 3 then it must go green while the neuter still asserts `SESSION_BUDGET_BLOCKS_BID`.

Run: `ctest --test-dir build -R tst_kalshi_bot_decision -V`
Expected before Step 3: compile error on `enforce_session_budget`.

- [ ] **Step 3: Add the Config flag**

In `KalshiBotDecision.h`, immediately after `double session_budget_usd = 120.00;`:

```cpp
        /// The session budget (issue #125) is a LIVE bounded-run safety: an
        /// armed run may commit at most this all-in before a human re-arms,
        /// and stop/resume does not reset it. The perpetual paper loop has no
        /// arming boundary, so enforcing a lifetime cap there is a deadlock —
        /// it bricks accumulation once cumulative paper all-in reaches the cap,
        /// long before the 300-settled gate. The paper loop (run_tick) sets
        /// this false; live (run_live_tick) leaves it true.
        bool enforce_session_budget = true;
```

- [ ] **Step 4: Guard the session-budget check in `decide()`**

In `KalshiBotDecision.cpp`, change the check at `:399`:

```cpp
        if (config.enforce_session_budget &&
            session_opened_usd + all_in > config.session_budget_usd + 1e-9) {
            refuse_on_cap(kSessionBudgetBlocksBid, session_opened_usd, config.session_budget_usd);
            continue;
        }
```

The `max_open_exposure_usd` check at `:395-398` is unchanged.

- [ ] **Step 5: Make the paper loop exempt itself**

In `KalshiBotCommands.cpp`, at the top of `run_tick` (`:308`, right after `const QString ledger_path = bot_ledger_path();`), introduce a paper-scoped config and use it for every `KalshiBotDecision::decide(...)` and `KalshiBotOrders::reconcile(...)` call within `run_tick` (the stopped-path `decide` at `:328`, `reconcile` at `:403`, and the main `decide` at `:428`):

```cpp
    // The lifetime session budget (issue #125) is a LIVE bounded-run safety;
    // the perpetual paper loop must not be bricked by it once cumulative paper
    // all-in reaches the cap. Paper is bounded by max_open_exposure_usd (open
    // risk) instead. Live keeps the budget (run_live_tick, default true).
    KalshiBotDecision::Config paper_config = config;
    paper_config.enforce_session_budget = false;
```

Replace the three `config` arguments named above with `paper_config`. Leave `run_live_tick` untouched.

- [ ] **Step 6: Run the tests to confirm they pass**

Run: `ctest --test-dir build -R tst_kalshi_bot_decision -V`
Expected: PASS, including the neuter still reporting `SESSION_BUDGET_BLOCKS_BID` under a default config.

- [ ] **Step 7: Commit**

```bash
git add openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotDecision.{h,cpp} \
        openmarketterminal-qt/src/cli/KalshiBotCommands.cpp \
        openmarketterminal-qt/tests/tst_kalshi_bot_decision.cpp
git commit -m "fix(kalshi-bot): paper loop is exempt from the lifetime session budget"
```

---

### Task 2: Void provably-orphaned paper positions (Fix 2)

**Files:**
- Modify: `openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotDecision.cpp` (`settle_paper` void branch + constants)
- Modify: `openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotOrders.cpp` (`replay` retires on void)
- Test: `openmarketterminal-qt/tests/tst_kalshi_bot_decision.cpp`

**Interfaces:**
- Consumes: a paper position row carries `ts_ms` (placement time, preserved from the original bid through `KalshiBotOrders.cpp:205 position = order.bid`), `position_id`, `side`, `contracts`, `stake_usd`, `fee_usd`.
- Produces: a `kalshi_bot_paper_void` event `{event, ts_ms, ts, mode:"paper", position_id, ticker, side, contracts, stake_usd, fee_usd, resolution:"unresolved_expired", reason, placed_ms, age_ms, won:null, realized_pnl:0.0}`. `KalshiBotOrders::replay` sets `order.settled = true` on it, exactly as for `kalshi_bot_paper_settlement`.

- [ ] **Step 1: Write the failing tests**

Add a small builder + tests to `tst_kalshi_bot_decision.cpp`. `kOrphanVoidAgeMs` is 48h; place the position at `kNow` and settle at `kNow + 49h` / `kNow + 1h` to straddle it.

```cpp
// A filled paper position as KalshiBotOrders::replay hands it to settle_paper.
QJsonObject open_position(const QString& ticker, qint64 placed_ms) {
    return QJsonObject{
        {QStringLiteral("action"), QStringLiteral("bid")},
        {QStringLiteral("ticker"), ticker},
        {QStringLiteral("side"), QStringLiteral("YES")},
        {QStringLiteral("contracts"), 3},
        {QStringLiteral("price"), 0.63},
        {QStringLiteral("stake_usd"), 1.89},
        {QStringLiteral("fee_usd"), 0.02},
        {QStringLiteral("ts_ms"), static_cast<double>(placed_ms)},
        {QStringLiteral("position_id"), ticker + QStringLiteral("@") + QString::number(placed_ms)}};
}
constexpr qint64 k49h = 49LL * 60 * 60 * 1000;
constexpr qint64 k1h = 60LL * 60 * 1000;
```

```cpp
// --- Fix 2: an aged orphan with no settlement record is voided, not held ---

void an_aged_orphan_with_no_settlement_is_voided_and_retired() {
    const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
    const QJsonArray open{open_position(ticker, kNow)};

    // No settlement records at all, position is 49h old (> 48h horizon).
    const QJsonArray voids = KalshiBotDecision::settle_paper(open, {}, kNow + k49h);
    QCOMPARE(voids.size(), 1);
    const QJsonObject v = voids.first().toObject();
    QCOMPARE(v.value(QStringLiteral("event")).toString(), QStringLiteral("kalshi_bot_paper_void"));
    QCOMPARE(v.value(QStringLiteral("resolution")).toString(), QStringLiteral("unresolved_expired"));
    QVERIFY(v.value(QStringLiteral("won")).isNull());
    QCOMPARE(v.value(QStringLiteral("realized_pnl")).toDouble(), 0.0);

    // Replaying [bid, fill, void] retires the FILLED position: nothing left
    // open, no exposure. Build the bid + fill rows in the exact shape
    // KalshiBotOrders::replay consumes — read its fill handling (KalshiBotOrders
    // .cpp: order established from the row carrying `order_state`, matched by its
    // id, then advanced by an `action:"fill"` row) and the existing
    // tst_kalshi_bot_orders fixtures; the id that the bid and fill share is the
    // `position_id`. Positive control first: [bid, fill] alone leaves ONE open
    // position with non-zero exposure, so the drop below is the void's doing.
    const QJsonArray filled_only = /* [bid(order_state), fill] for `ticker`@kNow */;
    const KalshiBotOrders::Book before = KalshiBotOrders::replay(filled_only);
    QCOMPARE(before.positions.size(), 1);
    QVERIFY(before.exposure_usd > 0.0);

    QJsonArray with_void = filled_only;
    with_void.append(v);
    const KalshiBotOrders::Book after = KalshiBotOrders::replay(with_void);
    QCOMPARE(after.positions.size(), 0);   // the void retired it
    QCOMPARE(after.exposure_usd, 0.0);
}

void a_matching_settlement_settles_and_never_voids_even_when_old() {
    const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
    const QJsonArray open{open_position(ticker, kNow)};
    const QJsonArray settlements{QJsonObject{{QStringLiteral("ticker"), ticker},
                                             {QStringLiteral("market_result"), QStringLiteral("YES")},
                                             {QStringLiteral("settled_time"), QStringLiteral("t")},
                                             {QStringLiteral("source"), QStringLiteral("kalshi-settlements.jsonl")}}};
    const QJsonArray rows = KalshiBotDecision::settle_paper(open, settlements, kNow + k49h);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toObject().value(QStringLiteral("event")).toString(),
             QStringLiteral("kalshi_bot_paper_settlement"));   // settled, not voided
}

void a_young_orphan_is_not_voided() {
    const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
    const QJsonArray open{open_position(ticker, kNow)};
    const QJsonArray rows = KalshiBotDecision::settle_paper(open, {}, kNow + k1h);  // 1h < 48h
    QCOMPARE(rows.size(), 0);   // stays open, no void
}

/// The gate and funnel count only kalshi_bot_paper_settlement
/// (KalshiBotGate.cpp:323, KalshiBotFunnel.cpp:84), so a void is a distinct
/// event and is never scored as a settled bid. Lock the event name in.
void a_void_is_not_a_settlement_event() {
    const QString ticker = QStringLiteral("KXBTCD-26JUL2808-T63499.99");
    const QJsonArray voids = KalshiBotDecision::settle_paper({open_position(ticker, kNow)}, {}, kNow + k49h);
    QVERIFY(voids.first().toObject().value(QStringLiteral("event")).toString() !=
            QStringLiteral("kalshi_bot_paper_settlement"));
}
```

- [ ] **Step 2: Run the tests to confirm they fail**

Run: `ctest --test-dir build -R tst_kalshi_bot_decision -V`
Expected: `an_aged_orphan...` fails — `settle_paper` currently returns 0 rows for an unmatched position (the `:512` stay-open branch), and `replay` does not know the void event.

- [ ] **Step 3: Add the void constant + horizon and the void branch in `settle_paper`**

In `KalshiBotDecision.cpp`, in the anonymous namespace beside `kSettlementEvent` (`:23`):

```cpp
constexpr auto kVoidEvent = "kalshi_bot_paper_void";
// 48h > any KXBTCD (~1h) / KXBTC15M (~15m) contract lifetime + the ~1-day
// settlement-record retention, so only a genuine orphan reaches the void.
constexpr qint64 kOrphanVoidAgeMs = 48LL * 60 * 60 * 1000;
```

Replace the stay-open branch (`:510-512`) with:

```cpp
        if (found == by_ticker.constEnd()) {
            // No settlement record. Young: a real settlement may still arrive
            // (matched first, above) — leave it open. Old past the horizon: a
            // genuine orphan whose market resolved long ago in a retained
            // window that rotated out. Retire it UNRESOLVED (never a fabricated
            // win/loss) so its exposure is released. Age is placement age
            // (TZ-free), not a ticker-parsed close.
            const qint64 placed_ms =
                static_cast<qint64>(position.value(QStringLiteral("ts_ms")).toDouble());
            if (placed_ms <= 0 || now_ms - placed_ms < kOrphanVoidAgeMs) continue;
            out.append(QJsonObject{
                {QStringLiteral("event"), QString::fromLatin1(kVoidEvent)},
                {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                {QStringLiteral("ts"), iso(now_ms)},
                {QStringLiteral("mode"), QStringLiteral("paper")},
                {QStringLiteral("live_eligible"), false},
                {QStringLiteral("position_id"), position.value(QStringLiteral("position_id"))},
                {QStringLiteral("ticker"), ticker},
                {QStringLiteral("side"), position.value(QStringLiteral("side"))},
                {QStringLiteral("contracts"), position.value(QStringLiteral("contracts"))},
                {QStringLiteral("stake_usd"), position.value(QStringLiteral("stake_usd"))},
                {QStringLiteral("fee_usd"), position.value(QStringLiteral("fee_usd"))},
                {QStringLiteral("resolution"), QStringLiteral("unresolved_expired")},
                {QStringLiteral("reason"),
                 QStringLiteral("position aged out with no settlement record in the retained window")},
                {QStringLiteral("placed_ms"), static_cast<double>(placed_ms)},
                {QStringLiteral("age_ms"), static_cast<double>(now_ms - placed_ms)},
                {QStringLiteral("won"), QJsonValue::Null},
                {QStringLiteral("realized_pnl"), 0.0}});
            continue;
        }
```

- [ ] **Step 4: Retire the position on the void event in `KalshiBotOrders::replay`**

In `KalshiBotOrders.cpp`, add the constant beside `kSettlementEvent` (`:19`):

```cpp
constexpr auto kVoidEvent = "kalshi_bot_paper_void";
```

Extend the settlement-retirement condition at `:136` so a void retires the order identically:

```cpp
        if (event == QLatin1String(kSettlementEvent) || event == QLatin1String(kVoidEvent)) {
```

(The body at `:138-139` — `it->settled = true;` and `resolved.insert(...)` — is unchanged.)

- [ ] **Step 5: Run the tests to confirm they pass**

Run: `ctest --test-dir build -R tst_kalshi_bot_decision -V`
Expected: PASS — orphan voids and is retired; a matched settlement still settles; a young orphan stays open; the void is not a settlement event.

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotDecision.cpp \
        openmarketterminal-qt/src/services/prediction/kalshi/KalshiBotOrders.cpp \
        openmarketterminal-qt/tests/tst_kalshi_bot_decision.cpp
git commit -m "fix(kalshi-bot): void provably-orphaned paper positions so exposure stops leaking"
```
