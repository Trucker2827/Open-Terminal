# openterminalcli Phase 2b — Widen Headless Tool Coverage (Design)

**Status:** Approved design (pending user spec review) — precedes the plan
**Date:** 2026-06-14
**Builds on:** Phase 2a (shipped on `main` at `3c68d97`) — `openterminal_core` lib, `HeadlessRuntime`, `--headless`, the two gates, and the `>= Verified` elevated-auth floor.

## Goal

Make the full read+write data-tool set actually **work** headless (not just be registered), by bringing up the same backing services the GUI does, and verifying each category end-to-end. The capability gating (core-only catalog, `cli.allow_trading`/`cli.allow_settings_write`, `>= Verified` floor) is already in place from 2a; 2b makes the data tools functional and confirms the gates hold across the widened set.

## Background (verified 2026-06-14)

- Tool handlers call `Service::instance()`; the singleton self-constructs. `ensure_registered_with_hub()` is a **separate, cheap** step that wires the service as a DataHub producer — it does **not** fetch (network happens only on an actual tool call).
- The GUI (`main.cpp`) calls `ensure_registered_with_hub()` for **11 services**: MarketData, News, Economics, DBnomics, MacroCalendar, Geopolitics, GovData, MAAnalytics, RelationshipMap, Maritime, AgentService.
- `HeadlessRuntime::init()` currently brings up only **MarketDataService**. So other data-category tools are registered but their producer isn't wired — peek/DataHub-backed paths are cold (and any tool that needs its service hub-registered won't fully work).
- `register_core_tools()` already yields a GUI-free catalog; the headless auth-checker already enforces the `>= Verified` floor + the two toggle gates, classifying settings-write vs trading off the live registry.

## Scope

- **Bring up all 11 GUI services** in `HeadlessRuntime::init()` (user chose full GUI parity).
- **Verify** each data category works headless (smoke), the capability gating holds across the widened set, and `cli.allow_settings_write` covers every settings-write tool.

### Non-goals
- `serve` daemon (Phase 3).
- GUI-control categories headless (navigation/workspace/dashboard — no window; stay GUI-only).
- Attach-mode CLI destructive (a revocable token — future).
- Re-tiering ExplicitConfirm tools (e.g. MCP-server provisioning) to be headless-reachable — deferred; the `>= Verified` floor correctly denies them for now.

## Design

### 1. Shared data-service bring-up (no drift)
Extract a shared `register_all_data_services()` helper (mirrors the existing `register_all_migrations()` pattern) that performs the 11 `ensure_registered_with_hub()` calls, declared where both callers reach it (e.g. a small `src/services/DataServices.{h,cpp}` in `SERVICE_SOURCES` → `openterminal_core`). `main.cpp` replaces its inline calls with it (verbatim set/order; GUI keeps any deferred-init nuance only if removing it changes behavior — otherwise the eager call is fine since registration is cheap). `HeadlessRuntime::init()` calls it after `Database::open` + `SecureStorage::init`.

### 2. One-shot safety for AgentService + Maritime (the one real risk)
Registration must be **non-blocking** in a one-shot CLI: it must not synchronously open a websocket (Maritime AIS feed) or spawn a process (AgentService Python). Verify each `ensure_registered_with_hub()` only wires the producer. If `MaritimeService`/`AgentService` registration does heavy/blocking/connecting work, guard it for headless: register the producer without starting the feed/subprocess (feeds/agents start only on explicit subscription/use, which a one-shot CLI doesn't trigger). **Acceptance:** `openterminalcli --headless version` (which runs full init) returns in well under a watchdog timeout with no spawned Python and no outbound AIS connection.

### 3. Capability gating — verify (mostly already done)
- `register_core_tools()` → catalog is GUI-free already. Confirm headless `mcp list` exposes only the intended data + system + settings-read categories, and no GUI category leaked in.
- The `>= Verified` floor + `cli.allow_trading` + `cli.allow_settings_write` gates apply unchanged to the widened set. Confirm a destructive tool in a newly-enabled category (e.g. a write tool) is still denied by default.
- If any non-data category is exposed that shouldn't be, add a category allowlist to the headless catalog/dispatch (only if needed — `register_core_tools` + gates likely suffice).

### 4. `cli.allow_settings_write` covers all settings-write tools
The classifier (`is_settings_write_tool`) is registry-based (`category=="settings" && is_destructive`). Confirm it catches **every** settings-write tool (enumerate `category=="settings"` tools; verify the writes are `is_destructive` and the reads are not). If a settings-write tool isn't flagged `is_destructive`, fix its definition so the gate covers it. Settings READ stays always-allowed.

### 5. Per-category headless smoke
For each enabled data category, call a representative tool headless and assert it RETURNS (data, or a clean network/no-data error — NOT a crash, hang, or "service not initialized"). Categories: markets, news, macro (economics/dbnomics/calendar), geopolitics, gov-data, M&A analytics, relationship-map, edgar, datahub, portfolio (read). Network-dependent results are acceptable as "returned without hanging" (sandbox may lack network); the gate is no-hang/no-crash + the in-process round-trip.

## Testing
- **QtTest:** `register_all_data_services()` brings up all 11 (assert each is registered with the hub / `is_active`); a headless smoke that init() completes fast (watchdog) with all 11 registered.
- **Per-category smoke** (shell or QtTest): each category's representative tool returns headless without hang/crash; a settings-write tool denied with `cli.allow_settings_write=false`, permitted when true; a destructive tool denied by default; a `>= Verified` tool denied regardless.
- **No-regression:** GUI builds + all `--selftest-*` exit 0 (the shared-helper extraction must not change GUI startup behavior); full ctest green; Phase-1 attach still works.

## Risks
- **AgentService/Maritime registration doing heavy work per invocation** — the main risk; mitigated by §2 (non-blocking verification + headless guard) and the fast-init acceptance test.
- **Shared-helper extraction changing GUI startup order** — mitigated by keeping the set/order verbatim and the GUI selftest no-regression gate.

## Open questions (non-blocking)
- Whether to expose a `--headless` cold-start time budget / lazy bring-up later if init proves slow with all 11 (defer; measure first).
