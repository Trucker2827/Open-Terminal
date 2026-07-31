# Kalshi bot cockpit — health-first redesign — design

**Date:** 2026-07-31 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-cockpit-health-redesign` (off `main`)

## Why

The cockpit leads with a green **"PAPER · BOT RUNNING · last decision 8s ago"**
banner whose only input is whether the *loop* is ticking. It says nothing about
whether the pipeline that feeds the loop is alive. On 2026-07-31 the Kalshi
market feed was **dead for hours** (WebSocket disconnected, then the market-data
REST timing out) while the cockpit still showed "BOT RUNNING" — the only hint
was a tiny "DATA ISSUE" badge on a different screen. The operator had to ask
"is data even harvested?" because the screen couldn't tell them.

Worse, three completely different situations all render the same way — as the
bot simply "not bidding":
- **broken** — feed down, calibrator stale, or the loop stopped (needs action);
- **no edge** — everything alive, but the model doesn't beat the market or no
  contract clears the threshold (the bot is *correctly* idle — nothing wrong);
- **acting** — fed, trusted, placing bids.

A cockpit that paints "feed dead" and "no edge right now" identically is the
core of the confusion. This redesign makes **pipeline health the loudest thing
on the screen**, and makes *broken* look nothing like *idle-by-design*.

## Goals & non-goals

**Goal:** a health-first banner at the top of the cockpit that answers, at a
glance, **is the bot alive and fed** — with three unambiguous states — while
keeping every detail the current cockpit shows.

**Non-goals / NEVER:**
- **Never render a broken pipeline as "running".** A dead feed, stale
  calibrator, or stopped loop must turn the banner RED with the failing stage
  named — it may not hide behind the loop ticking.
- **Never conflate "no edge" with "broken".** The bot correctly refusing to bet
  an untrusted signal is a normal AMBER state, not an alarm.
- **No teardown.** The edge-dot rain, the ledger stream, the "what the record
  teaches" lessons, and all five bottom panels (CALIBRATOR / SETTLEMENTS /
  SEALED GATE / KILL SWITCH / EXPOSURE) stay exactly as they are.
- **No new evidence collection.** The redesign reads signals that already exist
  on the evidence bus (`kalshi-ws-engine.json`, the ticker feed, the calibrator
  report, the ledger); it adds a reader, not a writer.

## The design

### The health banner (new top element)

Replaces the current single mood banner with a **4-stage pipeline strip** and one
overall headline:

```
● FEED DOWN — can't reach Kalshi (network), last data 4h ago · retrying     ⟵ RED
  HARVEST ●red 4h   CALIBRATE ●— stale-in   DECIDE ●ticking·stale   SETTLE ● 32
```
```
● NO EDGE — alive, model doesn't beat the market right now                   ⟵ AMBER
  HARVEST ●2s   CALIBRATE ●fresh·untrusted   DECIDE ●8s·passing   SETTLE ● 32
```
```
● LIVE & ACTING — bidding                                                    ⟵ GREEN
  HARVEST ●2s   CALIBRATE ●fresh·trusted   DECIDE ●8s·bidding   SETTLE ● 32
```

Each stage shows a colour dot + one datum. The **overall headline colour and
text** = the worst stage: RED names the broken stage and its reason; AMBER names
the honest reason it isn't betting; GREEN when acting.

### The three colours (the whole point)

| Colour | Meaning | When |
|--------|---------|------|
| 🔴 **RED — broken** | a stage can't do its job; needs attention | feed stale / WS disconnected / market REST failing; calibrator missing or stale; loop stopped; a cap/budget block that shouldn't bind |
| 🟡 **AMBER — honest wait** | everything works; there is genuinely nothing to bet | signal untrusted (Brier ≥ market); no contract clears the edge threshold; gate not yet passed (paper still accumulating) |
| 🟢 **GREEN — acting** | fed, trusted, placing bids | a bid was journaled within the last window |

### Per-stage classifier + thresholds

Computed in the pure presentation from inputs the view supplies. `now_ms` is the
render time.

- **HARVEST** — the market feed. Inputs: newest Kalshi market-event age
  (`kalshi-ws-engine.json` `last_event_at` / ticker-feed freshness) and
  `connected`. GREEN if a market event arrived < **60 s** ago and `connected`;
  AMBER if 60 s–5 min; **RED** if > **5 min** old or `connected == false`. When
  RED, surface `last_error` (e.g. "market-universe request timed out",
  "WebSocket disconnected") so the headline says *why*, not just *that*.
- **CALIBRATE** — the signal. Inputs: report `generated_at_ms` and
  `adds_value_over_market`. GREEN if report age < **120 s** (the bot's own
  `max_report_age_ms`) AND trusted; **AMBER** if fresh AND untrusted (honest
  no-edge); **RED** if age ≥ 120 s or the report is missing (stale/absent brain).
- **DECIDE** — the loop. Inputs: last-decision age and the dominant recent
  `reason_code`. GREEN if a `bid` was journaled recently; AMBER if ticking but
  passing (untrusted / edge-below-threshold / already-held / gate); **RED** if
  the loop is stopped (kill switch or no tick within ~2 intervals). A dominant
  `REPORT_STALE` is attributed **upstream** (it reddens HARVEST/CALIBRATE, not
  DECIDE) — the loop is fine; its input is stale.
- **SETTLE** — the record. Informational: settled count + last-settlement age.
  Never an alarm colour (GREY/GREEN); it is context, not health.

**Overall = RED if HARVEST ∨ CALIBRATE ∨ DECIDE is RED; else AMBER if any is
AMBER; else GREEN.** The headline text is the worst stage's reason.

### Failure taxonomy the banner must name

Each maps to a stage + colour so the operator knows *what to do*:
`FEED DOWN (network)` · `FEED DOWN (code)` · `CALIBRATOR STALE` · `SIGNAL
UNTRUSTED` (amber) · `NO EDGE` (amber) · `GATE NOT PASSED` (amber) · `LOOP
STOPPED` · `BUDGET/CAP BLOCKED`. The first two and the last are RED and
actionable; the amber ones are "working as designed".

## Architecture

`present_bot_cockpit(panel, report, gate, ledger_rows, live_status, now_ms,
grid_json, …)` (`BotCockpitPresentation.h`) stays a **pure function** — its
current virtue. It gains one input struct describing feed health:

```cpp
struct BotCockpitFeedHealth {
    bool     ws_connected = false;      ///< kalshi-ws-engine.json "connected"
    bool     credentials_ok = false;    ///< "credentials"
    qint64   newest_event_age_ms = -1;  ///< age of the newest Kalshi market event (-1 = none)
    QString  last_error;                ///< "connected" && last_error → surfaced when RED
    bool     readable = false;          ///< the engine file parsed at all (false = unknown/RED)
};
```

- The presentation computes the four stage states + overall banner from
  `feed` + the existing `report`/`panel`/ledger, and writes them onto
  `BotCockpitScene` (new fields: `health_stages[]` and a health-coloured
  `banner`/`banner_role`). The current mood/`live`/`dormant` fields stay for the
  rest of the scene; only the banner is re-derived health-first.
- **The view** (`KalshiBotCockpitView.cpp`) reads `kalshi-ws-engine.json` (and
  the newest ticker event) at the same cadence it already refreshes evidence,
  fills `BotCockpitFeedHealth`, and passes it to `present_bot_cockpit`. This is
  the only new I/O; it is read-only.
- **The renderer** (`KalshiBotCockpitView` paint / the CLI cockpit view) draws
  the 4-stage strip from `scene.health_stages` above the existing scene. Colour
  roles reuse the existing green/amber/red/grey vocabulary.

Everything below the banner is produced by the unchanged code paths.

## Testing (the pure presentation makes this cheap)

Unit tests over `present_bot_cockpit` — given fixture inputs, assert the banner
colour/text and each stage's colour. One case per row, each a real situation
from the record:

1. **Feed down (network)** — `ws_connected=false` / newest event 4 h old →
   banner **RED** "FEED DOWN", HARVEST red, and (neuter) the loop is still
   ticking paper, proving "running" no longer greens the banner.
2. **Feed down but loop ticking** — HARVEST red while DECIDE would otherwise be
   fine → overall RED (upstream wins).
3. **Signal untrusted** — feed fresh, report fresh, `adds_value=false` → banner
   **AMBER** "NO EDGE / SIGNAL UNTRUSTED", CALIBRATE amber, HARVEST green.
4. **Report stale** — report age ≥ 120 s → CALIBRATE **RED**, and a dominant
   `REPORT_STALE` reason does **not** redden DECIDE (attribution test).
5. **Acting** — feed fresh, trusted, a recent `bid` in the ledger → banner
   **GREEN** "LIVE & ACTING", all stages green.
6. **Loop stopped** — kill switch engaged → DECIDE **RED** "LOOP STOPPED",
   regardless of feed.
7. **Everything-below-unchanged** — the scene's columns/lessons/nodes for a
   given fixture are byte-identical to the pre-change output (a golden test that
   proves the redesign is additive).

Each test must fail before the change (the current banner has no health state)
and pass after.

## Rollout

1. `BotCockpitFeedHealth` struct + the stage classifier + banner, in the pure
   presentation, with tests 1–6.
2. The golden "additive" test (7) pinning the rest of the scene unchanged.
3. The view reads `kalshi-ws-engine.json` + newest-event age and passes the
   struct in; the renderer draws the strip.
4. Whole-branch review, then it ships in the next release — and the very outage
   that motivated it becomes a loud red banner instead of a question.
