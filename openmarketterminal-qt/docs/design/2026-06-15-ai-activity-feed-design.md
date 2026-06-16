# AI-Activity Feed — Design Spec

**Status:** Approved (design + toast-filter locked by user). **GUI feature; local-first.**
**Date:** 2026-06-15

## Goal

Make every AI/CLI trading action **visible to the human who armed the AI**, regardless of which screen they're on — so they are never "blind" to what the AI did. Two surfaces:
1. A **real-time toast** on each consequential action (shown on any screen).
2. A **persistent activity panel** (scrollable log of everything).

The data already exists: every gated AI trading action funnels through `TradeAuditRepository::append()` (the append-only `trade_audit` table: `ts, phase, tool, account, mode, intent_json, decision, reason, risk_snapshot_json`), and `TradeAuditRepository::recent(limit)` returns newest-first.

## Core idea: one chokepoint → fan out

Emit a **single EventBus signal from `TradeAuditRepository::append()`**. Because every AI trading action (paper + live, CLI + in-app agent, submit/fast_submit/cancel/replace/exit/prepare) already writes a `trade_audit` row, this one emit gives complete, real-time coverage with **zero changes to the individual tools**.

## Components

### 1. Emit (`TradeAuditRepository::append`)
After a successful row insert, publish `EventBus::instance().publish("trade.audit", row_to_map(row))` where the map carries `ts, phase, tool, account, mode, decision, reason, intent_json`. **Fire-and-forget and non-fatal**: the publish is wrapped so it can never make `append()` fail (the audit write is the source of truth; the signal is a convenience). `append()` runs on whatever thread the tool ran on (bridge worker for the CLI, agent thread, or main for the GUI agent).

### 2. `AiActivityNotifier` (GUI, main thread)
A small `QObject` owned by the shell (`WindowFrame`), created on the main thread at startup. It subscribes to `"trade.audit"`. **Cross-thread:** the subscription is delivered on the main thread (queued) so all UI work happens on the main thread even when `append()` fired on the bridge worker. On each event it:
- Runs the **pure formatter** `format_activity(row) -> ActivityView { bool toast; Severity severity; QString message; QString time, tool, account_mode, action, decision, reason; }`.
- If `toast == true`: `ToastService::instance().post(severity, message)` (shell-level → any frame shows it).
- Always: emits its own `Qt` signal `activity(ActivityView)` that the panel connects to (live prepend + flash).

### 3. Pure formatter `format_activity(const TradeAuditRow&) -> ActivityView` (the testable core)
**Toast rule (user-locked):** toast on **terminal/committed outcomes**, never on drafts or routine validation.
- `toast = (phase != "prepare") AND (decision ∈ TOAST_DECISIONS)`, where
  `TOAST_DECISIONS = { filled, partially_filled, accepted, submitted, new, open, cancelled, canceled, rejected, denied }` (case-insensitive).
- **Never toast:** `phase == "prepare"` (drafts), or validation-only decisions (`ok`, `draft`, `prepared`, `valid`, empty) — these are panel-only.
  > NOTE for spec review: `accepted`/`submitted`/`open`/`new` ARE toasted — these mean *the AI placed a real (resting) broker order*, which the human should see. If you want ONLY `filled/cancelled/rejected/denied` to toast (and a placed-but-unfilled order to stay panel-only), say so and we drop those four from `TOAST_DECISIONS`.
- **Severity:** `filled`/`partially_filled`/`cancelled`/`canceled` → `Success`; `accepted`/`submitted`/`new`/`open` → `Info`; `rejected`/`denied` → `Error`.
- **Message:** `"AI · <tool> · <action> → <decision> (<mode>)"`, e.g. `"AI · fast_submit_order · BUY 1 AAPL → filled (live)"`. `<action>` is a short summary parsed from `intent_json` (side + qty + symbol when present; else the tool's key fields). Robust to missing/garbled `intent_json` (falls back to the tool name).

### 4. `AiActivityFeed` panel (mirrors `PolymarketActivityFeed`)
A `QTableWidget` widget with columns: **Time · Tool · Account/Mode · Action · Decision · Reason**. On show, loads `TradeAuditRepository::recent(200)` (newest first). Connects to `AiActivityNotifier::activity` to **prepend** new rows live (cap 200, flash the new row briefly, decision-colored). Decision column colored (green/blue/red) via the same severity mapping.

### 5. Placement
A new **"AI Activity" tab in the Settings screen** (`SettingsScreen`), next to Security (where the AI is armed) and Notifications — reuses the existing settings-tab/section infra (`SecuritySection`, `NotificationsSection`). Low-risk, thematically adjacent. *(Alternative considered: a dedicated top-level nav screen — heavier wiring; deferred unless requested.)*

## Data flow
```
AI tool (CLI worker / agent / GUI)  →  TradeAuditRepository::append(row)
        → DB insert (source of truth)
        → EventBus.publish("trade.audit", map)        [fire-and-forget]
                → [main thread, queued] AiActivityNotifier
                        → format_activity(row)
                        → if toast: ToastService.post(...)        (any screen)
                        → emit activity(view) → AiActivityFeed prepend+flash
AiActivityFeed on open → TradeAuditRepository::recent(200)         (history)
```

## Error handling
- `append()` publish is wrapped (a publish exception/failure never breaks the audit insert or the tool).
- `recent()` load failure → empty table, logged, no crash.
- Malformed `intent_json` → formatter falls back to the tool name; never throws.
- Toast post + feed prepend are best-effort UI.

## Testing
- **Unit (pure):** `format_activity` — a matrix over decisions/phases: prepare-draft → no toast; `ok`/validation → no toast; `filled`/`accepted`/`cancelled`/`rejected`/`denied` → toast with the right `Severity`; message formatting from a representative `intent_json`; malformed `intent_json` → safe fallback. Neuter-proofable.
- **Unit (emit):** subscribe to `"trade.audit"` on the EventBus in a test, call `TradeAuditRepository::append(row)`, assert the event fired once with the row's fields. (Headless DB harness like `tst_settings_gate`.)
- **Build-verified + manual:** `AiActivityNotifier` shell wiring, the `AiActivityFeed` widget, the Settings tab, and the live toast — verified by building the GUI and (manual) running a CLI order → a toast appears + the row shows in the Settings → AI Activity tab.

## Files
- `src/storage/repositories/TradeAuditRepository.cpp` — emit `"trade.audit"` after insert (+ a `row_to_map` helper).
- `src/.../ai_activity/AiActivityFormat.{h,cpp}` — the pure `ActivityView` + `format_activity()` (testable; no Qt-widget deps).
- `src/.../ai_activity/AiActivityNotifier.{h,cpp}` — the main-thread QObject (subscribe → format → toast + signal).
- `src/screens/settings/AiActivitySection.{h,cpp}` — the feed panel (mirror `PolymarketActivityFeed`), wired into `SettingsScreen` as a tab.
- `src/app/WindowFrame*` — instantiate `AiActivityNotifier` at shell startup (main thread).
- `tests/tst_ai_activity.cpp` + `tests/CMakeLists.txt` — the two unit suites above.

## Out of scope (follow-ups)
- A dedicated top-level "AI Activity" nav screen (Settings tab first).
- Toasting non-trading AI actions (settings writes, data tools) — this feed is trading-only (`trade_audit`).
- Click-through from a feed row to the order/position detail.
- Persistent toast-history drawer (the `ToastService` already keeps a 100-item in-memory history; surfacing it is separate).
