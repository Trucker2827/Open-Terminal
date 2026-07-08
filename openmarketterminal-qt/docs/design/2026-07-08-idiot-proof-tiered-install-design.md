# Idiot-Proof Tiered Installation — Design Spec

**Date:** 2026-07-08
**Status:** Approved (brainstorming) — pending implementation plan
**Scope:** Make OpenTerminal's Python-dependent features (AI agents, exchange
daemon, chronos2 forecasting) work for end users who install the QtIFW
installer with **no Python** and **no chronos2 model** on their machine — via a
**tiered, opt-in, gracefully-degrading** setup rather than a hard prerequisite.

---

## Goal

A non-technical user runs the installer and the app **just works** immediately.
Nothing about Python, `pip`, `torch`, virtual environments, or model weights is
ever a prerequisite or a surprise. Heavy capabilities are opt-in, downloaded on
demand with clear progress, and their absence never crashes, hangs, or errors —
features that need them show a friendly "enable this" affordance instead.

## Non-goals (this spec)

- **No GPU / large-model path in v1.** CPU-only torch + the small Chronos-Bolt
  model is the default and only shipped profile. A GPU / larger-model upgrade is
  a later opt-in; this spec only leaves the seam for it.
- **No offline / air-gapped installer variant.** Enabling a tier requires
  internet once. The app *core* is always fully offline; users with no internet
  simply don't get the AI tiers. A "full offline bundle" is explicitly out of
  scope.
- **No change to what the Python features do** once enabled — this is about
  *delivery and gating*, not re-implementing agents, the daemon, or forecasting.
- **No bundling of Python/torch/model inside the installer.** The installer
  stays thin (Qt app only).

## Decisions (from brainstorming)

1. **Delivery model:** tiered graceful degradation. Small installer; app always
   usable; heavy capabilities are explicit opt-in downloads. Never blocks, never
   errors.
2. **Core Python (agents + daemon):** explicit **one-click opt-in**, not
   automatic. First launch does **not** silently download anything.
3. **Forecasting profile:** default **CPU-only torch + `amazon/chronos-bolt-small`**
   (~700 MB–1 GB), runs on any machine with no GPU. GPU/larger-model = later
   opt-in upgrade (seam only).
4. **Offline:** online-only opt-ins. App core fully offline; enabling a tier
   needs internet once, then works offline. Flaky/corporate networks handled via
   resumable + retried downloads and clear, host-named errors.

---

## The three tiers

| Tier | Name | Size | Requires | Unlocks |
|------|------|------|----------|---------|
| **0** | Core app | 0 (in installer) | — | Charts, watchlists, order book, crypto/portfolio trading, ladder — all non-Python UI. Always offline. |
| **1** | AI & Automation | ~300 MB | Tier 0 + internet once | `uv` → CPython 3.11 → core `requirements-numpy*` venvs. Agents, exchange daemon, research scripts, notebooks. |
| **2** | Forecasting | ~700 MB–1 GB | Tier 1 + internet once | CPU-only `torch` + `chronos-forecasting` + `amazon/chronos-bolt-small` weights in a dedicated `.venv-chronos` + managed HF cache. chronos2 forecast jobs. |

Tiers are **cumulative and ordered**: Tier 2 enablement first ensures Tier 1 is
ready (Python must exist before torch can be installed into a venv).

---

## Architecture

### Reused (existing infrastructure)

- **`PythonSetupManager`** (`src/python/PythonSetupManager.{h,cpp}`) — already
  downloads `uv` from Astral GitHub releases, installs CPython 3.11, creates
  venvs, installs `requirements-numpy*` with sha256 markers
  (`.packages_installed`), and emits progress. Today it is invoked automatically
  on first launch. **Change:** make it **tier-aware and opt-in-triggered**.
- **`SetupScreen`** (`src/screens/setup/SetupScreen.{h,cpp}`) — the existing
  first-run/setup UI wired in `main.cpp` to `PythonSetupManager::setup_complete`.
  **Change:** extend to present per-tier opt-in cards + progress, and to be
  dismissible (app usable without it).
- **`PythonRunner::is_available()`** — existing runtime check. Becomes one input
  to the new capability service.
- **`scripts/edge/chronos2_forecast.py`** — already loads
  `BaseChronosPipeline.from_pretrained(model, device_map=...)` and has a
  **mock-forecast fallback** when `chronos-forecasting` is absent. Kept as the
  safety net; unchanged except for the pinned default model + device.

### New components

- **`FeatureTier` capability service** (new,
  `src/python/FeatureTier.{h,cpp}` — exact path finalized in planning) — the pure
  authority on which tiers are **ready**. Computes tier state from:
  - Tier 1: `.setup_complete` sentinel + per-venv `.packages_installed` hash
    matches current requirements + `PythonRunner::is_available()`.
  - Tier 2: a new **`.chronos_ready`** marker recording the pinned model id +
    a torch-import verification.

  Exposes `TierState state(Tier)` → `{NotInstalled, Installing, Ready, Failed}`
  and a `tier_changed(Tier, TierState)` signal. **No UI, no download logic** — it
  reads sentinels and reports. Unit-tested against every sentinel combination.

- **Tier install orchestration** — extend `PythonSetupManager` with
  `ensure_tier(Tier)` that runs only the steps that tier needs and writes its
  sentinel atomically on success. Tier 2 adds a `.venv-chronos` venv installed
  from a new **`resources/requirements-chronos.txt`** (CPU-only torch via
  `--index-url https://download.pytorch.org/whl/cpu`, `chronos-forecasting`,
  `pandas`) and a model pre-fetch of `amazon/chronos-bolt-small` into a managed
  HF cache dir under the per-user install dir.

- **Enablement UI surfaces** —
  - Extend `SetupScreen` with two opt-in cards ("Enable AI & automation
    ~300 MB", then "Enable forecasting ~1 GB"), each showing live progress and a
    clear failure state with **Retry**.
  - **Inline affordances**: any Python-dependent surface (agent screens, daemon
    controls, the algo-trading chronos panels) asks `FeatureTier` and, when its
    tier is not ready, renders an "Enable to use this →" call-to-action that
    triggers `ensure_tier(...)` — discoverability at the point of need.

### Data flow

```
User clicks "Enable AI"  ─▶ PythonSetupManager.ensure_tier(Tier1)
                              │  download uv → install CPython → venvs → pkgs
                              │  atomic write .setup_complete / .packages_installed
                              ▼
                          FeatureTier recomputes ─▶ tier_changed(Tier1, Ready)
                              ▼
   agent/daemon UIs (subscribed) swap "Enable" affordance → live feature

User clicks "Enable forecasting" ─▶ ensure_tier(Tier2)  (ensures Tier1 first)
                              │  .venv-chronos ← requirements-chronos.txt (CPU torch)
                              │  pre-fetch amazon/chronos-bolt-small → HF cache
                              │  atomic write .chronos_ready(model id)
                              ▼
   chronos panels + edge chronos2 jobs enabled
```

### Controls & behavior

- **First launch:** app opens fully functional (Tier 0). A **dismissible** setup
  card offers Tier 1; no download starts without an explicit click.
- **Enable click:** preflight (free disk, reachability of GitHub/PyPI/HF) → clear
  go/no-go → download with resumable progress → atomic sentinel on success.
- **Feature access while tier off:** the feature surface shows an "enable"
  affordance, never a raw error or hang.
- **After enablement:** tiers persist across app restarts and app updates
  (version-pinned; no re-download unless a requirements hash changes).

### Error / edge handling

- **Network drop mid-download:** downloads are resumable + retried; on give-up,
  a clear "couldn't reach `<host>` — Retry" state (the tier returns to
  `NotInstalled`/`Failed`, never a corrupt half-state).
- **Corporate proxy / firewall block:** the failure names the exact host that
  failed (github.com / pypi.org / huggingface.co / download.pytorch.org) so the
  user (or their IT) knows what to allowlist.
- **Insufficient disk:** preflight refuses to start with the required-vs-free
  numbers, rather than failing halfway.
- **Killed / half-finished setup:** completion sentinels are written **only** on
  full success and verified (`uv pip list`), so a re-run is idempotent and
  self-heals a partial install.
- **Forecast invoked before Tier 2 ready:** `chronos2_forecast.py`'s existing
  mock-forecast fallback returns a clearly-labeled mock rather than crashing.
- **No internet at all:** Tier 0 is unaffected; Tier 1/2 cards explain internet
  is needed once and offer Retry later.

---

## Testing

- **Unit (`FeatureTier`):** given every combination of sentinels present/absent/
  stale-hash, `state(Tier)` returns the correct `{NotInstalled, Installing,
  Ready, Failed}`; `tier_changed` fires on transitions. Requirements-hash marker
  logic (match / mismatch / missing) is covered.
- **Manual / integration on a fresh machine with no Python:**
  1. Install via QtIFW → app opens, Tier 0 features work, no download occurred.
  2. Enable Tier 1 → agents/daemon become available; sentinel written.
  3. Enable Tier 2 → chronos panels + `edge chronos2` forecast work; model cached.
  4. Restart app → tiers still ready, no re-download.
  5. **Kill the network mid-download** → resumes / clean Retry, no corrupt state.
  6. **Simulate a proxy block** → clear host-named error + Retry.
  7. Access an AI/forecast feature while its tier is off → "enable" affordance,
     never a crash or raw error.

The `PythonSetupManager` tier orchestration is verified by the manual
fresh-machine matrix above (it spawns real `uv`/network operations); `FeatureTier`
carries the automated unit coverage.

---

## Files

- Create: `src/python/FeatureTier.{h,cpp}` (pure capability service) + unit test
  `tests/tst_feature_tier.cpp` (register in `tests/CMakeLists.txt`).
- Create: `resources/requirements-chronos.txt` (CPU torch + chronos-forecasting +
  pandas) and add to the packaging/copy step so it ships with the app.
- Modify: `src/python/PythonSetupManager.{h,cpp}` — add `ensure_tier(Tier)`,
  the `.venv-chronos` + model pre-fetch, `.chronos_ready` sentinel, preflight
  checks, and make setup opt-in-triggered rather than auto on first launch.
- Modify: `src/screens/setup/SetupScreen.{h,cpp}` — tiered opt-in cards, live
  progress, Retry/failure states, dismissible.
- Modify: `src/app/main.cpp` — stop auto-triggering setup on first launch; show
  the dismissible Tier-1 card instead.
- Modify: Python-dependent UI surfaces (agent screens, daemon controls,
  `src/screens/algo_trading/*` chronos panels) — query `FeatureTier` and render
  the inline "enable" affordance when a tier is not ready.
- Modify: `scripts/edge/chronos2_forecast.py` — pin the default model to
  `amazon/chronos-bolt-small` and honor the managed HF cache dir (behavior
  otherwise unchanged; mock fallback retained).

## Phasing (for the plan)

1. **Tiering + gating core:** `FeatureTier` + `ensure_tier` refactor + make setup
   opt-in (Tier 0/1 working, degradation everywhere). Ship-able on its own.
2. **Forecasting tier:** `requirements-chronos.txt`, `.venv-chronos`, model
   pre-fetch, `.chronos_ready`, chronos panel affordances.
3. **Robustness polish:** preflight (disk/reachability), resumable/retry, host-
   named errors, partial-install self-heal.

## Out of scope (explicit)

- GPU / CUDA torch, larger Chronos models, model selection UI.
- Offline / air-gapped installer or downloadable full bundle.
- Re-implementing or changing agent / daemon / forecasting behavior.
- Bundling Python, torch, or model weights inside the QtIFW installer.
