# Kalshi bot cockpit — health-first redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a health-first pipeline banner to the bot cockpit that distinguishes *broken* (red) from *no-edge* (amber) from *acting* (green), so a dead feed can never render as green "BOT RUNNING".

**Architecture:** `present_bot_cockpit(...)` stays a pure function; it gains a read-only `BotCockpitFeedHealth` input and emits new `BotCockpitScene` fields (`health_stages`, `health_banner`, `health_role`). The classifier is pure and unit-tested. The view fills the feed-health struct from `kalshi-ws-engine.json` + newest-ticker age and the renderer draws a 4-stage strip. Purely additive — existing `mood`/`banner`/columns/nodes/pulses are untouched.

**Tech Stack:** C++20, Qt6 (QJsonObject/Array), QtTest.

## Global Constraints

- **Additive only.** Do NOT change `scene.mood`, `scene.banner`, `scene.columns`, `scene.nodes`, `scene.pulses`, `scene.lessons`, or the five bottom panels. Add NEW fields and a NEW top strip. A golden test pins the rest of the scene byte-identical.
- **Broken ≠ idle-by-design.** RED only for a stage that cannot do its job (feed stale/disconnected, calibrator stale/missing, loop stopped). AMBER for honest waits (untrusted signal, edge-below-threshold, gate-not-passed). GREEN only when a bid was journaled recently.
- **A stale *report* reddens upstream, never DECIDE** — the loop is not blamed for its input.
- **Read-only.** The only new I/O is reading `kalshi-ws-engine.json` + the newest ticker event; no evidence is written.
- Anchors: pure presentation `present_bot_cockpit` at `openmarketterminal-qt/src/screens/kalshi/BotCockpitPresentation.h` (signature ~line 355; `BotCockpitScene` struct ~line 154; the live/paper/dormant classifier ~line 370). View + renderer: `openmarketterminal-qt/src/screens/kalshi/KalshiBotCockpitView.cpp`. Tests: `openmarketterminal-qt/tests/tst_kalshi_bot_cockpit.cpp` (helpers `calibrator_report()`, `evaluated_gate()`, `panel_for()`; `present_bot_cockpit(panel_for(ledger), report, gate, ledger, live_status, now_ms)`).

---

### Task 1: Health classifier + banner in the pure presentation

**Files:**
- Modify: `openmarketterminal-qt/src/screens/kalshi/BotCockpitPresentation.h`
- Test: `openmarketterminal-qt/tests/tst_kalshi_bot_cockpit.cpp`

**Interfaces:**
- Produces: input `struct BotCockpitFeedHealth`; output `struct BotCockpitHealthStage` + `QList<BotCockpitHealthStage> BotCockpitScene::health_stages`, `QString health_banner`, `QString health_role`. `present_bot_cockpit(...)` gains a trailing `const BotCockpitFeedHealth& feed = {}` parameter (defaulted, so every existing caller and test compiles unchanged).

- [ ] **Step 1: Add the structs + scene fields**

In `BotCockpitPresentation.h`, near the other cockpit structs:

```cpp
/// Feed/harvest health the pure presentation cannot see for itself — supplied
/// by the view from kalshi-ws-engine.json + the newest retained ticker event.
struct BotCockpitFeedHealth {
    bool    readable = false;            ///< the engine status parsed at all (false => UNKNOWN)
    bool    ws_connected = false;        ///< "connected"
    bool    credentials_ok = false;      ///< "credentials"
    qint64  newest_event_age_ms = -1;    ///< age of the newest Kalshi market event (-1 => none)
    QString last_error;                  ///< surfaced in the headline when HARVEST is red
};

/// One stage of the health strip. `role` is a colour vocabulary already used by
/// the scene: "green" | "amber" | "red" | "grey".
struct BotCockpitHealthStage {
    QString id;      ///< "harvest" | "calibrate" | "decide" | "settle"
    QString label;   ///< "HARVEST" ...
    QString value;   ///< the one datum ("2s", "fresh·trusted", "8s·bidding", "32")
    QString role = QStringLiteral("grey");
};
```

Add to `BotCockpitScene`:

```cpp
    QList<BotCockpitHealthStage> health_stages;  ///< HARVEST -> CALIBRATE -> DECIDE -> SETTLE
    QString health_banner;                       ///< the health-first headline (worst stage)
    QString health_role = QStringLiteral("grey");///< colour of the headline
```

- [ ] **Step 2: Write the failing tests**

Add to `tst_kalshi_bot_cockpit.cpp` (uses existing `calibrator_report`, `evaluated_gate`, `panel_for`; add a small `feed()` helper). Trusted vs untrusted report is `adds_value_over_market`; recency uses `generated_at_ms`.

```cpp
// Feed-health fixtures.
BotCockpitFeedHealth feed_live()   { return {true, true, true, 2'000, {}}; }
BotCockpitFeedHealth feed_down()   { return {true, false, true, 14'400'000,
                                             QStringLiteral("WebSocket disconnected")}; }
QString stage_role(const BotCockpitScene& s, const QString& id) {
    for (const auto& st : s.health_stages) if (st.id == id) return st.role;
    return QStringLiteral("missing");
}
```

```cpp
// 1. Feed down turns the banner RED even though the loop is ticking paper.
void feed_down_reddens_the_banner_over_a_ticking_loop() {
    const QJsonArray ledger = bidding_ledger();            // existing helper producing a running paper loop
    const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), /*trusted=*/true);
    const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                  QByteArray(), kBotCockpitMaxColumns,
                                                  kBotCockpitMaxPulses, feed_down());
    QCOMPARE(s.health_role, QStringLiteral("red"));
    QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("red"));
    QVERIFY(s.health_banner.contains(QStringLiteral("FEED")));
    // Neuter: the same inputs with a LIVE feed are not red.
    const BotCockpitScene ok = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                   QByteArray(), kBotCockpitMaxColumns,
                                                   kBotCockpitMaxPulses, feed_live());
    QVERIFY(ok.health_role != QStringLiteral("red"));
}

// 2. Fresh feed + fresh-but-untrusted report => AMBER "no edge", not red.
void untrusted_signal_is_amber_not_red() {
    const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), /*trusted=*/false);
    const QJsonArray ledger = passing_ledger();           // ticking, passing
    const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                  QByteArray(), kBotCockpitMaxColumns,
                                                  kBotCockpitMaxPulses, feed_live());
    QCOMPARE(s.health_role, QStringLiteral("amber"));
    QCOMPARE(stage_role(s, QStringLiteral("harvest")), QStringLiteral("green"));
    QCOMPARE(stage_role(s, QStringLiteral("calibrate")), QStringLiteral("amber"));
}

// 3. Report older than 120s => CALIBRATE red, and a REPORT_STALE loop does NOT redden DECIDE.
void stale_report_reddens_calibrate_not_decide() {
    const QJsonObject report = calibrator_report(kNow - 200'000, one_trusted_prediction(), true);
    const QJsonArray ledger = report_stale_ledger();      // decisions dominated by REPORT_STALE
    const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                  QByteArray(), kBotCockpitMaxColumns,
                                                  kBotCockpitMaxPulses, feed_live());
    QCOMPARE(stage_role(s, QStringLiteral("calibrate")), QStringLiteral("red"));
    QVERIFY(stage_role(s, QStringLiteral("decide")) != QStringLiteral("red"));
}

// 4. Fed + trusted + a recent bid => GREEN acting.
void acting_is_green() {
    const QJsonObject report = calibrator_report(kNow, one_trusted_prediction(), true);
    const QJsonArray ledger = bidding_ledger();
    const BotCockpitScene s = present_bot_cockpit(panel_for(ledger), report, {}, ledger, {}, kNow,
                                                  QByteArray(), kBotCockpitMaxColumns,
                                                  kBotCockpitMaxPulses, feed_live());
    QCOMPARE(s.health_role, QStringLiteral("green"));
    for (const auto& id : {"harvest","calibrate","decide"})
        QCOMPARE(stage_role(s, QString::fromLatin1(id)), QStringLiteral("green"));
}

// 5. Kill switch => DECIDE red "LOOP STOPPED", regardless of feed.
void stopped_loop_reddens_decide() {
    KalshiBotStopFile stop; stop.engaged = true; stop.ts_ms = kNow - 5'000;
    const QJsonArray ledger = bidding_ledger();
    const BotCockpitScene s = present_bot_cockpit(panel_for(ledger, {}, {}, kNow, stop),
                                                  calibrator_report(kNow, one_trusted_prediction(), true),
                                                  {}, ledger, {}, kNow, QByteArray(),
                                                  kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed_live());
    QCOMPARE(stage_role(s, QStringLiteral("decide")), QStringLiteral("red"));
    QVERIFY(s.health_banner.contains(QStringLiteral("STOPPED")));
}

// 6. Unknown feed (view supplied nothing) => HARVEST grey/unknown, never a false green.
void unknown_feed_is_not_green() {
    const BotCockpitScene s = present_bot_cockpit(panel_for(bidding_ledger()),
                                                  calibrator_report(kNow, one_trusted_prediction(), true),
                                                  {}, bidding_ledger(), {}, kNow);  // default feed {}
    QVERIFY(stage_role(s, QStringLiteral("harvest")) != QStringLiteral("green"));
}
```

Where `one_trusted_prediction()`, `bidding_ledger()`, `passing_ledger()`, `report_stale_ledger()` reuse or lightly extend the fixtures already in the test file; add any missing one beside the existing helpers. If `calibrator_report` has no `trusted` parameter, add one that sets `adds_value_over_market`.

- [ ] **Step 3: Run the tests — confirm they fail**

Run: `ctest --test-dir openmarketterminal-qt/build -R tst_kalshi_bot_cockpit -V`
Expected: compile error (no `health_stages`/`BotCockpitFeedHealth`) then, after Step 1, assertion failures (no classifier yet).

- [ ] **Step 4: Implement the classifier + banner**

In `present_bot_cockpit`, after the existing mood block, compute the four stages (pure, from `feed`, `report`, `panel`, `ledger_rows`, `now_ms`) and set `scene.health_stages`, `scene.health_banner`, `scene.health_role`:

- HARVEST: `!feed.readable` → grey "unknown"; `!feed.ws_connected` or `newest_event_age_ms < 0` or `> 300'000` → **red** (banner "FEED DOWN — " + `last_error`); `> 60'000` → amber; else green ("<n>s").
- CALIBRATE: report age = `now_ms - generated_at_ms`. missing or `>= 120'000` → **red** "CALIBRATOR STALE"; fresh && `adds_value_over_market` → green "fresh·trusted"; fresh && !trusted → **amber** "fresh·untrusted".
- DECIDE: from `panel`/ledger — stopped (kill switch / no recent tick) → **red** "LOOP STOPPED"; recent `bid` → green "bidding"; ticking+passing → amber "passing". A dominant `REPORT_STALE` does not colour DECIDE red (it is HARVEST/CALIBRATE's fault).
- SETTLE: grey/green info — settled count.
- Overall: `health_role` = red if any of harvest/calibrate/decide is red; else amber if any amber; else green. `health_banner` = the worst stage's reason.

`mood`, `banner`, and the rest of the scene are left exactly as they are.

- [ ] **Step 5: Run the tests — confirm they pass**

Run: `ctest --test-dir openmarketterminal-qt/build -R tst_kalshi_bot_cockpit -V`
Expected: PASS, neuters included.

- [ ] **Step 6: Add the additive golden test**

Assert that for a representative fixture, the pre-existing scene fields are unchanged by the redesign — `columns`, `nodes`, `pulses`, `lessons`, `census`, `mood`, `banner` — so the redesign is provably additive. (Snapshot the relevant fields before wiring is impossible post-hoc; instead assert their concrete expected values for the fixture, matching the current output.)

- [ ] **Step 7: Commit**

```bash
git add openmarketterminal-qt/src/screens/kalshi/BotCockpitPresentation.h \
        openmarketterminal-qt/tests/tst_kalshi_bot_cockpit.cpp
git commit -m "feat(kalshi): health-first classifier + banner in the pure bot cockpit"
```

---

### Task 2: View plumbing + renderer draws the strip

**Files:**
- Modify: `openmarketterminal-qt/src/screens/kalshi/KalshiBotCockpitView.cpp`
- (If a CLI cockpit renderer exists — e.g. `CommandDispatch.cpp`'s `kalshi bot cockpit` — render the strip there too.)

**Interfaces:**
- Consumes: `BotCockpitScene::health_stages` / `health_banner` / `health_role` from Task 1; `BotCockpitFeedHealth` to fill.

- [ ] **Step 1: Read feed health from the evidence bus**

At the cockpit view's existing data-poll (where it already reads `calibrator.json`, `kalshi-bot-gate.json`, the ledger, and `live_status`), also read `kalshi-ws-engine.json` and the newest retained ticker event. Fill a `BotCockpitFeedHealth`:
- `readable` = the engine JSON parsed;
- `ws_connected` = `connected`; `credentials_ok` = `credentials`;
- `newest_event_age_ms` = `now - last_event_at` (fall back to the newest `kalshi-tickers.jsonl` row ts, or `-1` if none);
- `last_error` = `last_error`.

Pass it as the new trailing argument to `present_bot_cockpit(...)`.

- [ ] **Step 2: Render the 4-stage strip**

Draw `scene.health_stages` as a strip at the very top of the cockpit (above the current banner), each stage a coloured dot + `label` + `value`, and render `scene.health_banner` in `scene.health_role`'s colour as the headline. Reuse the existing colour-role → QColor mapping the cockpit already uses for nodes/pulses.

- [ ] **Step 3: Build + run the app; verify by eye against live states**

Run: build the GUI target and launch; confirm the strip shows GREEN/AMBER/RED correctly against the live bot (fed+trusted → green/amber; kill the feed briefly → red). Also run the existing cockpit tests to confirm no regression:
`ctest --test-dir openmarketterminal-qt/build -R 'tst_kalshi_bot_cockpit|tst_command_dispatch' -V`

- [ ] **Step 4: Commit**

```bash
git add openmarketterminal-qt/src/screens/kalshi/KalshiBotCockpitView.cpp
git commit -m "feat(kalshi): cockpit view reads feed health and draws the pipeline strip"
```
