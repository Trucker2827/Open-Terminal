# openterminalcli Phase 2 — Headless Mode Design Spec

**Status:** Approved design (pending user spec review) — precedes the implementation plan
**Date:** 2026-06-14
**Builds on:** Phase 1 (`docs/design/2026-06-14-openterminalcli-design.md`, shipped on `main` at `b8db197`).

## Goal

Give `openterminalcli` a true `--headless` mode that brings the terminal brain up **in-process** under `QCoreApplication` — no running GUI required — so most read/write MCP data tools (plus `selftest`, `quote`, `hub`) run from the shell, CI, or cron. The same command tree works whether attached to a running GUI or headless.

## Scope

- **Broad data-tool coverage headless** (user's choice): the full read+write data-tool surface, with the deliberate gate below.
- **The CLI is a full read+write operator tool**, user-controlled — NOT read-only. The one hard gate is the destructive / trading-execution tier (§4).
- **Extract `openterminal_core`** (a `Qt6::Core/Network/Sql/Concurrent` static lib) that both the GUI exe and `openterminalcli` link.
- **Decouple `DataHub` from QtWidgets** so it lives in the core lib.

### Non-goals
- `serve` daemon (long-lived headless DataHub + feeds) — Phase 3.
- GUI-control categories (`navigation`/`workspace`/`dashboard`) running headless — they have no window; they remain attach-only.
- Bypassing the app's deterministic trading risk limits / kill-switch — out of scope and explicitly preserved.

## Background — feasibility (verified in the codebase, 2026-06-14)

- **Services are essentially Widgets-free:** 1 of 206 `src/services`+`src/trading`+`src/algo_engine` `.cpp` files includes a QtWidgets header (`src/services/workflow/adapters/ServiceBridges.cpp`). `auth`, `network`, `storage` groups: 0 GUI files.
- **`DataHub`'s only QtWidgets coupling** is `is_owner_active_for_work(QObject* owner)` (`src/datahub/DataHub.cpp:42-46`): it `qobject_cast<QWidget*>`es the owner to find its top-level window for visibility-based work-gating, and **already returns `true` for any non-widget owner (every service)**. Headless wants exactly that no-op path.
- **`McpProvider` is `QCoreApplication`-safe** (`qApp->thread()` resolves under a core app; no Widgets).
- **`ToolConfirmationGate` is already core-clean** — the modal presenter is injected (`set_presenter(std::function<bool(QString,QString)>)`) and it **denies if no presenter is installed**.
- **Tools carry a `category` field** (`src/mcp/McpTypes.h:265,381`) and there is a `ToolFilter` with category include/exclude — the mechanism for headless capability gating.
- A handful of `CORE_SOURCES` files are GUI-coupled and stay GUI-side: `WorkspaceShell.cpp`, `SymbolDragSource.cpp`, `builtin_actions.cpp`, `WindowCycler.cpp`, `DockLayoutSelftest.cpp`. The ~10 GUI MCP tool files (`WorkspaceTools*`, `NavigationTools`, `DashboardTools`, `ExcelTools`, `DataHubPeekHelpers`) stay GUI-side too.

## Architecture

### `openterminal_core` static lib
Links `Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent` (+ optional `Qt6::WebSockets`) and the static deps `miniz`, `openmarketterminal_ed25519`. **No `Qt6::Widgets`/`Gui`.** Source groups, by triage (the linker enforces correctness — a stray Widgets include fails the lib link):

| Group | Into core lib? |
|---|---|
| NETWORK, DATAHUB (decoupled), STORAGE, PYTHON, LLM_SERVICE, TRADING, ALGO_ENGINE | whole |
| SERVICE | whole **minus** `workflow/adapters/ServiceBridges.cpp` (GUI parts → GUI-side shim) |
| AUTH | minus `lock/LockOverlayController.cpp` |
| CORE | minus `WorkspaceShell`, `SymbolDragSource`, `builtin_actions`, `WindowCycler`, `DockLayoutSelftest` |
| MCP | core dispatch + data tools; minus the ~10 GUI tool files |
| UI, SCREEN, AI_CHAT, APP | stay in the GUI exe |

The GUI exe and `openterminalcli` both `target_link_libraries(... openterminal_core)`. The GUI additionally links `Qt6::Widgets` + the GUI-only groups.

### DataHub decoupling — injected hook (decision)
`DataHub` gains a member `std::function<bool(QObject*)> owner_active_hook_` defaulting to `[](QObject*){ return true; }`. `is_owner_active_for_work` calls the hook instead of touching `QWidget`. `DataHub.cpp` drops `#include <QWidget>` → Core-only. The **GUI** installs the real QWidget-`window()` implementation at startup (e.g. in `main.cpp` after `QApplication`); headless leaves the default (always-active), which is the current behavior for non-widget owners anyway. (Rejected alternatives: a GUI decorator subclass, or `#ifdef` two builds — more machinery, no gain.)

### Tool registration split (decision)
Split `McpInit` into `register_core_tools()` (in the lib) and `register_gui_tools()` (GUI exe). The GUI calls both; headless calls only `register_core_tools()`. Headless additionally applies a **category allowlist** via the existing `ToolFilter` so only the §4 default-on categories are exposed, and installs a `ToolConfirmationGate` presenter that enforces the destructive gate (§4).

### Headless runtime — one-shot (decision)
A `HeadlessRuntime` in the core lib performs, under a `QCoreApplication`: open `Database`/`CacheDatabase` → `SecureStorage::init()` → run migrations → install DataHub (default hook) → bring up the headless service set → `register_core_tools()` + apply the capability filter + install the destructive-gate presenter → dispatch one command → teardown. No persistent window/event loop; the process exits after the command (the event loop runs only as long as needed to resolve async fetches, mirroring the Phase-1 `run_async_wait` pattern).

### Mode selection (decision)
`--headless` is **explicit**. Default behavior is unchanged (attach to a running GUI via Phase-1 `bridge.json`). `--headless` forces the in-process runtime. The command tree (`status`, `mcp`, `hub`, `quote`, `selftest`, `version`) is identical in both modes; only the transport behind it differs (HTTP-to-GUI vs in-process `McpProvider`). Not auto-fallback — too magical; a user who means headless says so.

## Capability tiers (the category table)

| Category | CLI status | Notes |
|---|---|---|
| `markets` | ✅ default on | quotes, instruments, market data |
| `quotes` | ✅ default on | subset of markets (`get_quote`, …) |
| `news` | ✅ default on | |
| `portfolio` (read) | ✅ default on | holdings/positions read |
| `analytics` | ✅ default on | quant / MA / surface analytics |
| `datahub` (read) | ✅ default on | `peek`, `list_topics`, `request` |
| `edgar` | ✅ default on | SEC filings |
| `macro` | ✅ default on | FRED / DBnomics / calendar |
| `geopolitics` | ✅ default on | GDELT |
| `selftest` | ✅ default on | headless self-tests |
| `navigation` | ✅ default on — **attach only** | drives a running GUI; N/A in `--headless` (no window) |
| `workspace` | ✅ default on — **attach only** | panels/layouts; attach only |
| `dashboard` | ✅ default on — **attach only** | attach only |
| `system` | ✅ default on | version, migrate, logs, health |
| `settings` (read) | ✅ default on | read config |
| `settings` (write) | ⛔ **off unless GUI toggle on** | write config — gated by `cli.allow_settings_write` |
| **`destructive` / `trading-execution`** | ⛔ **off unless GUI toggle on** | live orders, Python exec, workflow/agent mutation — gated by `cli.allow_trading` |

Reads are always on (the user owns the tool). GUI-control categories simply don't apply headless. Two capabilities are gated behind GUI setting toggles: **config writes** and **trading/destructive**.

### The two gated capabilities (GUI setting toggles)

Two persistent settings, **both default `false`**, are the authoritative gates. They are flipped by the user **in the GUI Settings screen** (toggle switches); the CLI *reads* them and obeys, and may also surface/flip them via `openterminalcli config get/set` — but the GUI toggle is the canonical control surface the human owns.

| Setting | Gates | Default |
|---|---|---|
| `cli.allow_settings_write` | CLI writing config (`settings` write tools) | false |
| `cli.allow_trading` | CLI destructive / trading-execution tools | false |

**Settings read** is never gated.

**Enforcement point** (same model for both, so a raw HTTP client can't bypass it):
- **Headless mode:** the `HeadlessRuntime` checks the settings and installs/configures the dispatch gate accordingly — destructive tools allowed only when `cli.allow_trading`; settings-write tools allowed only when `cli.allow_settings_write`; otherwise the gate denies (exit 5 with an "enable in GUI Settings (or `config set …`)" message).
- **Attach mode:**
  - *Trading/destructive* — **out of scope (deferred to a future phase).** `bridge.json` carries **no** `destructive_token`, so the attach-mode CLI is read + non-destructive and structurally cannot send `X-MCP-Allow-Destructive`. (The original 2a plan of writing the token only when `cli.allow_trading` is true was dropped: the CLI had no consumer for it in 2a, making it an unconsumed, non-revocable replay secret — see the Task 6 security fix. The in-process `destructive_token_` still exists for in-app Python agents and is never written to disk.) The working trading gate is **headless** mode via `cli.allow_trading` + the `HeadlessRuntime` auth-checker, above.
  - *Settings write* — settings-write tools are gated server-side (bridge/`McpProvider`) on `cli.allow_settings_write`, so the running GUI itself refuses CLI-originated config writes when the toggle is off.

**Hard constraint (preserved):** `cli.allow_trading` lifts only the CLI's own gate. It does **NOT** bypass the app's deterministic risk limits, live-enable flag, or kill-switch (`PositionManager` / `DeploymentRunner` / `UnifiedTrading` paper-default) — the "AI/CLI can't lift its own leash" safety floor stays intact.

**Standing-capability note (accepted by user):** these opt-ins are persistent — while a toggle is on, a compromised shell/agent could use that capability without a fresh confirmation. Mitigations: both default false, flipped only by a deliberate human GUI action; `status` surfaces both toggle states; recommend turning them off when not in use.

## CMake extraction details
- New `add_library(openterminal_core STATIC ...)` aggregating the in-lib source groups; `set_target_properties(... AUTOMOC ON)`; carry over `SKIP_UNITY_BUILD_INCLUSION` items (e.g. `KeychainKey.cpp`) and all `target_compile_definitions` feature flags that the moved sources need.
- The GUI `OpenMarketTerminal` target drops the moved groups from its own source list and links `openterminal_core` (keeps UI/SCREEN/AI_CHAT/APP + the GUI-only CORE/AUTH/MCP files + the GUI-side DataHub-hook install + `register_gui_tools()`).
- `openterminalcli` links `openterminal_core` and gains `--headless` (Phase-1 attach code unchanged).
- Unity build: the lib gets its own unity batches; verify the moved TUs still unity-compile (watch for ODR/include-order issues the monolith masked).

## Headless init sequence (what `HeadlessRuntime` replicates from `main.cpp`)
DB open → `SecureStorage::init()` (machine-derived key, no keychain prompt — already the default) → `MigrationRunner` → DataHub ready (default hook) → for the requested command, bring up the **minimal services it needs** (e.g. `quote` → `MarketDataService` + `HttpClient`; `hub` → DataHub only; broad tools → their services), each via `ensure_registered_with_hub()` / lazy init → `register_core_tools()` + capability filter + gate presenter → dispatch → teardown. Services are brought up lazily per command where practical to keep cold-start fast.

## Error handling & exit codes
Reuse Phase 1: `0` ok · `2` usage · `3` no instance (attach mode only) · `4` token rejected (attach) · `5` tool `success:false` · `6` transport/HTTP (attach). Headless adds: `7` headless init failure (DB/migration/service bring-up). A destructive tool blocked by the gate reports a clear "enable with `config set cli.allow_trading true`" message and exits `5`.

## Testing
- **QtTest:** `HeadlessRuntime` init/teardown (DB+DataHub come up, a core tool is callable, teardown clean); the DataHub-hook decoupling (default hook returns active; injected hook honored); the capability filter (a GUI/destructive category is excluded/denied; `cli.allow_trading` toggles the gate).
- **Per-service headless smoke:** `openterminalcli --headless quote AAPL`, `--headless hub topics`, `--headless mcp call <data tool>` return real data; a destructive tool is denied when `cli.allow_trading=false` and allowed when true.
- **No-GUI-regression gate (critical):** after the extraction, the GUI exe still builds and every existing `--selftest-*` passes, and the Phase-1 attach path still works — proving the lib split didn't change GUI behavior.

## Build sequencing (de-risks "broad")
The end state is broad, but the plan builds **foundation-first**:
1. **Foundation + vertical slice:** create `openterminal_core`, decouple DataHub, split `McpInit`, build `HeadlessRuntime`, wire `--headless`, and prove with `quote`/`hub`/`selftest` + the GUI no-regression gate.
2. **Widen:** register the full read+write data-tool set, bring up each backing service headless, vet each for `QCoreApplication`-safety, and wire the `cli.allow_trading` gate end-to-end.

This may be split into two implementation plans (2a foundation, 2b widen); each is independently shippable and testable. One design spec governs both.

## Risks
- **CMake surgery on a 2,900-line unity build** — mechanical but fiddly (link order, unity batching, feature-flag defs). The GUI no-regression gate is the backstop.
- **Service init ordering headless** — a service that assumes GUI-time init order could misbehave; foundation-first + per-service smoke surfaces this early.
- **Standing `cli.allow_trading`** — accepted; mitigated by default-off + `status` visibility + the untouched deterministic risk floor.

## GUI Settings additions (required for the toggles)
The GUI Settings screen gains two toggle switches — **"Allow CLI to write settings"** (`cli.allow_settings_write`) and **"Allow CLI trading / destructive actions"** (`cli.allow_trading`) — both default off. These are the canonical control surface; the CLI reads the same keys.

## Open questions (non-blocking)
- Exact lazy-vs-eager service bring-up per command (perf tuning, decided during 2b).
- Whether to add a per-invocation `--allow-trading` override later in addition to the persistent toggle.
