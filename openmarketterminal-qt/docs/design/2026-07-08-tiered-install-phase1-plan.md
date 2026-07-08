# Idiot-Proof Tiered Installation — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OpenTerminal open **fully usable with zero Python downloaded**, and turn Python setup into an explicit, dismissible **one-click opt-in** (Tier 1 = AI & automation), with a pure capability service that every Python-dependent surface can query to degrade gracefully.

**Architecture:** Add a pure `FeatureTier` service that maps the existing `PythonSetupManager::check_status()` (`SetupStatus`) + `PythonRunner::is_available()` + a new `.chronos_ready` sentinel onto three tier states, unit-tested in isolation. Stop `main.cpp` from gating startup on `SetupScreen`; always launch the main window and expose a dismissible in-app "Enable AI & automation" affordance that triggers the existing setup. Wire one representative Python-dependent surface to the graceful-degradation affordance to prove the pattern.

**Tech Stack:** Qt6/C++17, QtTest+ctest, `uv`-based `PythonSetupManager` (existing), custom QWidgets.

**Reference spec:** `docs/design/2026-07-08-idiot-proof-tiered-install-design.md`

## Global Constraints

- **Opt-in, never automatic:** first launch downloads **nothing**; the app is fully usable (Tier 0) before any Python exists. Setup begins only on an explicit user click.
- **Never blocks, never errors:** a Python-dependent feature whose tier is not ready shows an "enable" affordance — never a crash, hang, or raw error.
- **DRY over existing infra:** reuse `PythonSetupManager::check_status()`/`run_setup()` and `PythonRunner::is_available()`. Do **not** reinvent venv/marker logic; `FeatureTier` only *reads/maps* existing state.
- **Tier names (verbatim):** Tier 0 = "Core app"; Tier 1 = "AI & Automation"; Tier 2 = "Forecasting".
- **Tier states (verbatim enum):** `NotInstalled`, `Installing`, `Ready`, `Failed`.
- **Phase 1 does not implement the Tier 2 install** (chronos venv/model) — it only models Tier 2's *state* (reads a `.chronos_ready` sentinel that Phase 2 will write) so the UI can already show it as `NotInstalled`.
- **No new third-party deps.** Qt only.
- **Namespace:** `openmarketterminal::python` for the service (matches `PythonSetupManager`).

---

### Task 1: `FeatureTier` pure capability service

**Files:**
- Create: `src/python/FeatureTier.h`
- Create: `src/python/FeatureTier.cpp`
- Test: `tests/tst_feature_tier.cpp`
- Modify: `tests/CMakeLists.txt` (register the test), `CMakeLists.txt` (compile `FeatureTier.cpp` into both the app and CLI source lists next to `PythonSetupManager.cpp`)

**Interfaces:**
- Consumes: `openmarketterminal::python::SetupStatus` (from `PythonSetupManager.h`) fields `needs_setup`, `venv_numpy1_ready`, `venv_numpy2_ready`, `needs_package_sync`, `install_dir`.
- Produces:
  - `enum class Tier { Core, Ai, Forecasting };`
  - `enum class TierState { NotInstalled, Installing, Ready, Failed };`
  - Free function `TierState tier_state_from(Tier, const SetupStatus&, bool python_available, bool chronos_ready, bool installing);` — **pure**, no I/O, unit-tested.
  - Class `FeatureTier` (QObject singleton) with `TierState state(Tier) const;`, `void refresh();`, signal `void tier_changed(Tier, TierState);`. `state()`/`refresh()` call `check_status()` + `PythonRunner::is_available()` + `chronos_ready()` and delegate to `tier_state_from`.

**Design notes (mapping rules — implement exactly):**
- `Tier::Core` → always `Ready` (no Python needed).
- `Tier::Ai` → `Installing` if `installing` is true; else `Ready` if `!status.needs_setup && status.venv_numpy1_ready && status.venv_numpy2_ready && python_available`; else `NotInstalled`. (There is no persisted "Failed" for Ai in Phase 1 — failure is surfaced transiently by the installer UI, so map non-ready to `NotInstalled`.)
- `Tier::Forecasting` → `Installing` if `installing`; else `Ready` if Ai is `Ready` **and** `chronos_ready`; else `NotInstalled`.
- `chronos_ready()` (member): returns `QFileInfo(status.install_dir + "/.chronos_ready").exists()`. Phase 1 never writes this file, so it is always false → Forecasting shows `NotInstalled`.

- [ ] **Step 1: Write the failing test** — `tests/tst_feature_tier.cpp` (APPLESS, pure-function coverage of `tier_state_from`):

```cpp
#include "python/FeatureTier.h"
#include <QtTest>
using namespace openmarketterminal::python;

class TestFeatureTier : public QObject {
    Q_OBJECT
  private slots:
    void coreAlwaysReady();
    void aiReadyOnlyWhenFullyInstalled();
    void aiInstallingWins();
    void forecastingNeedsAiPlusChronos();
};

static SetupStatus mk(bool needs_setup, bool v1, bool v2) {
    SetupStatus s;
    s.needs_setup = needs_setup;
    s.venv_numpy1_ready = v1;
    s.venv_numpy2_ready = v2;
    s.install_dir = "/tmp/x";
    return s;
}

void TestFeatureTier::coreAlwaysReady() {
    QCOMPARE(tier_state_from(Tier::Core, mk(true, false, false), false, false, false), TierState::Ready);
    QCOMPARE(tier_state_from(Tier::Core, mk(false, true, true), true, true, true), TierState::Ready);
}

void TestFeatureTier::aiReadyOnlyWhenFullyInstalled() {
    // Fully installed + python available -> Ready
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, true, true), true, false, false), TierState::Ready);
    // Any missing piece -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Ai, mk(true, true, true), true, false, false), TierState::NotInstalled);
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, false, true), true, false, false), TierState::NotInstalled);
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, true, true), false, false, false), TierState::NotInstalled);
}

void TestFeatureTier::aiInstallingWins() {
    // installing flag overrides everything for Ai
    QCOMPARE(tier_state_from(Tier::Ai, mk(true, false, false), false, false, true), TierState::Installing);
}

void TestFeatureTier::forecastingNeedsAiPlusChronos() {
    auto ready = mk(false, true, true);
    // Ai ready but no chronos -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, false, false), TierState::NotInstalled);
    // Ai ready + chronos -> Ready
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, true, false), TierState::Ready);
    // chronos true but Ai not ready -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Forecasting, mk(true, false, false), false, true, false), TierState::NotInstalled);
    // installing overrides
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, true, true), TierState::Installing);
}

QTEST_APPLESS_MAIN(TestFeatureTier)
#include "tst_feature_tier.moc"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R feature_tier --output-on-failure`
Expected: FAIL to compile — `FeatureTier.h` not found.

- [ ] **Step 3: Write `src/python/FeatureTier.h`**

```cpp
#pragma once
// Pure tier-capability mapping over the existing PythonSetupManager state.
// Reports which capability tier is ready so UI can gracefully degrade.
#include "python/PythonSetupManager.h"

#include <QObject>

namespace openmarketterminal::python {

enum class Tier { Core, Ai, Forecasting };
enum class TierState { NotInstalled, Installing, Ready, Failed };

// Pure mapping — no I/O. `installing` reflects an in-flight ensure_tier().
TierState tier_state_from(Tier tier, const SetupStatus& status, bool python_available,
                          bool chronos_ready, bool installing);

class FeatureTier : public QObject {
    Q_OBJECT
  public:
    static FeatureTier& instance();

    TierState state(Tier tier) const;
    bool is_ready(Tier tier) const { return state(tier) == TierState::Ready; }

    // Mark a tier as installing / cleared, so state() reports Installing while
    // PythonSetupManager runs. Emits tier_changed.
    void set_installing(Tier tier, bool installing);

    // Recompute from live status and emit tier_changed for any transitions.
    void refresh();

  signals:
    void tier_changed(Tier tier, TierState state);

  private:
    explicit FeatureTier(QObject* parent = nullptr);
    Q_DISABLE_COPY_MOVE(FeatureTier)
    bool chronos_ready() const;

    bool installing_ai_ = false;
    bool installing_forecasting_ = false;
};

} // namespace openmarketterminal::python
```

- [ ] **Step 4: Write `src/python/FeatureTier.cpp`**

```cpp
#include "python/FeatureTier.h"
#include "python/PythonRunner.h"

#include <QFileInfo>

namespace openmarketterminal::python {

TierState tier_state_from(Tier tier, const SetupStatus& status, bool python_available,
                          bool chronos_ready, bool installing) {
    if (tier == Tier::Core)
        return TierState::Ready;
    if (installing)
        return TierState::Installing;
    const bool ai_ready = !status.needs_setup && status.venv_numpy1_ready &&
                          status.venv_numpy2_ready && python_available;
    if (tier == Tier::Ai)
        return ai_ready ? TierState::Ready : TierState::NotInstalled;
    // Tier::Forecasting
    return (ai_ready && chronos_ready) ? TierState::Ready : TierState::NotInstalled;
}

FeatureTier& FeatureTier::instance() {
    static FeatureTier inst;
    return inst;
}

FeatureTier::FeatureTier(QObject* parent) : QObject(parent) {}

bool FeatureTier::chronos_ready() const {
    const auto status = PythonSetupManager::instance().check_status();
    return QFileInfo::exists(status.install_dir + "/.chronos_ready");
}

TierState FeatureTier::state(Tier tier) const {
    const auto status = PythonSetupManager::instance().check_status();
    const bool py = PythonRunner::instance().is_available();
    const bool installing = (tier == Tier::Ai) ? installing_ai_ : installing_forecasting_;
    return tier_state_from(tier, status, py, chronos_ready(), installing);
}

void FeatureTier::set_installing(Tier tier, bool installing) {
    if (tier == Tier::Ai)
        installing_ai_ = installing;
    else if (tier == Tier::Forecasting)
        installing_forecasting_ = installing;
    emit tier_changed(tier, state(tier));
}

void FeatureTier::refresh() {
    emit tier_changed(Tier::Ai, state(Tier::Ai));
    emit tier_changed(Tier::Forecasting, state(Tier::Forecasting));
}

} // namespace openmarketterminal::python
```

- [ ] **Step 5: Register in CMake + tests, run test to verify it passes**

Add `src/python/FeatureTier.cpp` to the app and CLI target source lists in `CMakeLists.txt` (next to `src/python/PythonSetupManager.cpp`). Add a `tst_feature_tier` test target in `tests/CMakeLists.txt` mirroring the `tst_crypto_ladder_model` entry.

Run: `cmake --build build && ctest --test-dir build -R feature_tier --output-on-failure`
Expected: PASS (4/4).

- [ ] **Step 6: Prove the test gates**

Neuter `tier_state_from`'s Ai branch (`return ai_ready ? ... : ...` → `return TierState::Ready;`), rebuild the test, confirm `aiReadyOnlyWhenFullyInstalled` FAILS, then restore and confirm PASS.

- [ ] **Step 7: Commit**

```bash
git add src/python/FeatureTier.h src/python/FeatureTier.cpp tests/tst_feature_tier.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(setup): FeatureTier pure capability service (tiered, unit-tested)"
```

---

### Task 2: Stop gating startup on setup; always launch the app (Tier 0)

**Files:**
- Modify: `src/app/main.cpp:935-1002` (the `if (setup_status.needs_setup) { … return app.exec(); }` block)

**Interfaces:**
- Consumes: nothing new. Removes the blocking first-run branch.
- Produces: the app always reaches the normal main-window path regardless of `setup_status.needs_setup`.

**Design notes:** The main-window construction path already exists immediately below the removed block (lines 1004+). Deleting the early-return branch makes every launch fall through to it. Do **not** call `run_setup()` here — Phase 1 is opt-in. Keep the existing `needs_package_sync` background-sync branch (that only runs when Python is *already* installed and a requirements hash changed — still desirable and never downloads Python from scratch). Leave `SetupScreen` class in place; Task 3 repurposes it as a non-blocking surface.

- [ ] **Step 1: Remove the blocking branch**

Delete the entire `if (setup_status.needs_setup) { … return app.exec(); }` block (main.cpp:935-1002). Replace with a one-line log so intent is explicit:

```cpp
    // Tiered install: the app is fully usable with no Python (Tier 0). We do
    // NOT gate startup on setup or auto-download anything — enabling the AI
    // tier is an explicit in-app opt-in (see the enable-AI affordance). The
    // needs_package_sync branch below still runs ONLY when Python already
    // exists and a requirements hash changed (never a cold download).
    if (setup_status.needs_setup)
        LOG_INFO("App", "Python not installed — starting in Tier 0 (AI features opt-in)");
```

- [ ] **Step 2: Build**

Run: `cmake --build build`
Expected: rc 0.

- [ ] **Step 3: Manual verify (fresh state)**

Temporarily rename the install dir so `needs_setup` is true (`mv "$HOME/Library/Application Support/org.openterminal.OpenTerminal" /tmp/ot-bak` on macOS), launch the app, confirm the **main window opens directly** (no setup gate) and no `uv`/Python download starts (watch: no network, no `.setup_complete` appears). Restore the dir afterward.

- [ ] **Step 4: Commit**

```bash
git add src/app/main.cpp
git commit -m "feat(setup): open app in Tier 0 without gating on Python setup"
```

---

### Task 3: In-app "Enable AI & Automation" opt-in surface

**Files:**
- Create: `src/screens/setup/EnableTierBanner.h`
- Create: `src/screens/setup/EnableTierBanner.cpp`
- Modify: `CMakeLists.txt` (compile `EnableTierBanner.cpp` into the app target)
- Modify: `src/app/WindowFrame.cpp` (host the banner at the top of the primary window when `Tier::Ai` is not ready) — exact insertion point identified during implementation; follow how other top-of-window strips are added.

**Interfaces:**
- Consumes: `openmarketterminal::python::FeatureTier` (`state`, `set_installing`, `tier_changed`) and `PythonSetupManager` (`run_setup`, `progress_changed`, `setup_complete`).
- Produces: a dismissible `QWidget` banner: label "Enable AI & automation (agents, daemon) — one-time ~300 MB download", an **[Enable]** button, a **[Not now]** (dismiss) button, and an inline `QProgressBar` shown while installing.

**Design notes / behavior:**
- On construct: if `FeatureTier::instance().state(Tier::Ai) == Ready`, hide self. Otherwise show the offer.
- **[Enable]** → `FeatureTier::instance().set_installing(Tier::Ai, true)`; connect `PythonSetupManager::progress_changed` → update the progress bar/label; connect `setup_complete(success,err)` → on success `set_installing(false)` + `refresh()` + hide banner; on failure `set_installing(false)`, show inline "Couldn't set up — Retry" with the **[Enable]** button relabeled "Retry". Then call `PythonSetupManager::instance().run_setup()`.
- **[Not now]** → hide banner for this session (no persisted state needed in Phase 1).
- Never modal; the window stays fully interactive while installing.

- [ ] **Step 1: Write `EnableTierBanner.h`**

```cpp
#pragma once
// Dismissible top-of-window opt-in strip for enabling the AI & automation tier.
#include "python/FeatureTier.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

namespace openmarketterminal::python { struct SetupProgress; }

namespace openmarketterminal::screens {

class EnableTierBanner : public QWidget {
    Q_OBJECT
  public:
    explicit EnableTierBanner(QWidget* parent = nullptr);

  private slots:
    void on_enable_clicked();
    void on_progress(const openmarketterminal::python::SetupProgress& p);
    void on_setup_done(bool success, const QString& error);

  private:
    void refresh_visibility();
    QLabel* msg_ = nullptr;
    QPushButton* enable_btn_ = nullptr;
    QPushButton* dismiss_btn_ = nullptr;
    QProgressBar* bar_ = nullptr;
    bool installing_ = false;
};

} // namespace openmarketterminal::screens
```

- [ ] **Step 2: Write `EnableTierBanner.cpp`** (full implementation)

```cpp
#include "screens/setup/EnableTierBanner.h"
#include "python/PythonSetupManager.h"

#include <QHBoxLayout>

namespace openmarketterminal::screens {
using python::FeatureTier;
using python::Tier;
using python::TierState;

EnableTierBanner::EnableTierBanner(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    msg_ = new QLabel(tr("Enable AI & automation (agents, daemon) — one-time ~300 MB download."), this);
    enable_btn_ = new QPushButton(tr("Enable"), this);
    dismiss_btn_ = new QPushButton(tr("Not now"), this);
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setVisible(false);
    bar_->setFixedWidth(160);
    row->addWidget(msg_, 1);
    row->addWidget(bar_);
    row->addWidget(enable_btn_);
    row->addWidget(dismiss_btn_);

    connect(enable_btn_, &QPushButton::clicked, this, &EnableTierBanner::on_enable_clicked);
    connect(dismiss_btn_, &QPushButton::clicked, this, [this] { hide(); });
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::progress_changed,
            this, &EnableTierBanner::on_progress);
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::setup_complete,
            this, &EnableTierBanner::on_setup_done);
    refresh_visibility();
}

void EnableTierBanner::refresh_visibility() {
    setVisible(FeatureTier::instance().state(Tier::Ai) != TierState::Ready);
}

void EnableTierBanner::on_enable_clicked() {
    if (installing_) return;
    installing_ = true;
    FeatureTier::instance().set_installing(Tier::Ai, true);
    enable_btn_->setEnabled(false);
    dismiss_btn_->setEnabled(false);
    bar_->setVisible(true);
    bar_->setValue(0);
    msg_->setText(tr("Setting up AI & automation…"));
    python::PythonSetupManager::instance().run_setup();
}

void EnableTierBanner::on_progress(const python::SetupProgress& p) {
    if (!installing_) return;
    bar_->setValue(p.progress);
    if (!p.message.isEmpty())
        msg_->setText(p.message);
}

void EnableTierBanner::on_setup_done(bool success, const QString& error) {
    if (!installing_) return;
    installing_ = false;
    FeatureTier::instance().set_installing(Tier::Ai, false);
    bar_->setVisible(false);
    dismiss_btn_->setEnabled(true);
    if (success) {
        FeatureTier::instance().refresh();
        hide();
    } else {
        enable_btn_->setEnabled(true);
        enable_btn_->setText(tr("Retry"));
        msg_->setText(tr("Couldn't set up AI features: %1").arg(error));
    }
}

} // namespace openmarketterminal::screens
```

- [ ] **Step 3: Host the banner in `WindowFrame`** — add an `EnableTierBanner` as the first row of the primary window's central layout (top strip). It self-hides when `Tier::Ai` is ready, so it costs nothing once installed. Build.

Run: `cmake --build build`
Expected: rc 0.

- [ ] **Step 4: Manual verify** — with a fresh state (no Python), launch: the banner appears at the top of the window. Click **Enable** → progress bar advances, message updates; on completion the banner disappears and agents/daemon become available. Relaunch → banner does not reappear (Tier Ai Ready). Click **Not now** on a fresh state → banner hides for the session; app remains fully usable.

- [ ] **Step 5: Commit**

```bash
git add src/screens/setup/EnableTierBanner.h src/screens/setup/EnableTierBanner.cpp src/app/WindowFrame.cpp CMakeLists.txt
git commit -m "feat(setup): dismissible Enable-AI opt-in banner (Tier 1)"
```

---

### Task 4: Graceful-degradation affordance on a Python-dependent surface

**Files:**
- Create: `src/screens/setup/TierGate.h` (small reusable helper widget)
- Create: `src/screens/setup/TierGate.cpp`
- Modify: `CMakeLists.txt` (compile `TierGate.cpp`)
- Modify: one representative agent surface (identified during implementation — e.g. the agents list panel under `src/screens/*agent*`), to show `TierGate` when `Tier::Ai` is not ready instead of its normal empty/error state.

**Interfaces:**
- Consumes: `FeatureTier` (`state`, `tier_changed`).
- Produces: `TierGate` — a centered placeholder widget with a message ("AI features aren't enabled yet") and an **[Enable AI & automation]** button that triggers the same `EnableTierBanner` enable path via a shared slot `TierGate::request_enable()` which calls `FeatureTier::instance().set_installing(Tier::Ai, true)` + `PythonSetupManager::instance().run_setup()`. It subscribes to `tier_changed` and emits `becameReady()` so the host surface can swap it out for the live feature.

**Design notes:** `TierGate` is the reusable graceful-degradation primitive; Phase 1 wires it into exactly one surface to prove the pattern (further surfaces are folded in during Phase 2/3, not here). It must never show a raw error; if `run_setup()` fails it shows "Couldn't set up — Retry".

- [ ] **Step 1: Write `TierGate.h` / `TierGate.cpp`** — mirror `EnableTierBanner`'s enable/progress/done handling but rendered as a centered full-panel placeholder with a `becameReady()` signal. (Full code follows the same shape as Task 3's `EnableTierBanner.cpp`; reuse the same `PythonSetupManager` signal connections and the `installing_` guard.)

- [ ] **Step 2: Wire into the chosen agent surface** — where the surface currently assumes Python/agents exist, guard with `if (!FeatureTier::instance().is_ready(Tier::Ai)) show TierGate else show live view;` and connect `TierGate::becameReady` → rebuild the live view.

- [ ] **Step 3: Build + manual verify** — fresh state: open the agent surface → see the `TierGate` placeholder (no crash/error). Click **Enable AI & automation** → progress → on completion the live agents view appears. Build rc 0.

- [ ] **Step 4: Commit**

```bash
git add src/screens/setup/TierGate.h src/screens/setup/TierGate.cpp CMakeLists.txt <agent-surface-file>
git commit -m "feat(setup): TierGate graceful-degradation affordance; wire agents surface"
```

---

## Self-Review

**Spec coverage (Phase 1 slice):**
- "App opens fully usable (Tier 0), nothing downloaded" → Task 2. ✅
- "Explicit one-click opt-in for core Python" → Task 3. ✅
- "Capability/gating service, unit-tested" → Task 1. ✅
- "Never crash/hang/error when a tier is off; enable affordance at point of need" → Task 4. ✅
- "Model Tier 2 state (chronos) without implementing its install" → Task 1 (`.chronos_ready` read; always NotInstalled). ✅
- Deferred to Phase 2/3 (documented, not gaps): the actual Tier 2 install (`requirements-chronos.txt`, `.venv-chronos`, model prefetch, `.chronos_ready` write), preflight/resumable/host-named errors, wiring the remaining Python surfaces, and pinning `chronos2_forecast.py`'s model/cache.

**Placeholder scan:** Task 4's Step 1 references "full code follows the same shape as Task 3" — acceptable only because Task 3 contains the complete code and Task 4 is a structural mirror; the implementer has the concrete template. The one-representative-surface file is intentionally chosen at implementation time (the codebase has several agent panels; the reviewer confirms the choice).

**Type consistency:** `Tier`/`TierState` enums, `tier_state_from(...)` signature, `FeatureTier` members, and `PythonSetupManager` signals (`progress_changed`, `setup_complete`, `run_setup`, `check_status`) match the real `PythonSetupManager.h` interface read during planning.

---

## Follow-on plans (not this plan)

- **Phase 2 — Forecasting tier:** `resources/requirements-chronos.txt` (CPU torch + chronos-forecasting + pandas), `PythonSetupManager::ensure_tier(Forecasting)` that builds `.venv-chronos` and pre-fetches `amazon/chronos-bolt-small` into a managed HF cache, writes `.chronos_ready`; `chronos2_forecast.py` pins the model + honors the cache dir; chronos algo-trading panels get `TierGate`.
- **Phase 3 — Robustness polish:** preflight (free disk, host reachability), resumable/retried downloads, host-named failure messages, partial-install self-heal, and folding `TierGate` into the remaining Python-dependent surfaces.
