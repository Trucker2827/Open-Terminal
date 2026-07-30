# Kalshi Strategy-Grid Consumers (CLI + Bot Line) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface `kalshi-strategy-grid-latest.json` read-only in the CLI and the bot cockpit — survivors AND top candidates (near-misses) with their humility metadata — staleness-aware and fail-closed, without recompute, gate-coupling, or loosening the survivor bar.

**Architecture:** A small upstream Python change makes the grid emit `candidates[]`. One pure C++ core (`KalshiStrategyGridView`) parses `-latest.json` and formats both a one-line cockpit string and a CLI report; a thin CLI verb and a thin cockpit line call it. Mirrors the repo's "pure presentation over evidence files" pattern (`BotCockpitPresentation`).

**Tech Stack:** Python 3 stdlib (Task 1); C++17 / Qt (`QJsonDocument`), QtTest (Tasks 2–4); the existing CLI dispatch (`CommandDispatch.cpp`, `kalshi_evidence_path`).

## Global Constraints

- **Read-only** — consumers never write, recompute, or call the grid; they parse the evidence file only.
- **Fail closed** — a missing / unreadable / garbage / schema-mismatched file reads `UNAVAILABLE`; a file older than `GRID_STALE_MS = 6h` reads `(STALE …)` but still shows its numbers. Never a confident blank or a zero (the issue-#145 discipline).
- **Never upgrade a candidate to a winner** — only `trust == "measured"` (set by the engine's gate) is a survivor; the consumer cannot promote.
- **Candidate rule (pre-registered):** top `CANDIDATE_TOP_N = 8` NON-survivor variants with `delta_vs_hold > 0 AND delta_vs_market > 0 AND effective_n >= CANDIDATE_MIN_EFF_N (=10)`, ranked by `delta_vs_market` descending. Each carries `blocked_by`, chosen by precedence **effective_n → BH significance → walk-forward**.
- **No gate/trade coupling; no maker; no autonomous path.**
- **Python paths at repo-root `scripts/research/`; C++ under `openmarketterminal-qt/`.** Run Python tests from `openmarketterminal-qt/`; the C++ test target is `OpenMarketTerminal`'s test harness (mirror an existing `tst_kalshi_*`).

---

### Task 1: Grid emits `candidates[]` in `-latest.json`

**Files:**
- Modify: `scripts/research/strategy_grid.py` (repo root) — add constants + `_blocked_by` + `candidates` in `latest_summary`.
- Test: `openmarketterminal-qt/tests/test_strategy_grid.py`.

**Interfaces:**
- Produces: `latest_summary(full)` output gains a `candidates` list; each item = `{variant_id, side, band, gate, exit, delta_vs_hold, delta_vs_market, effective_n, ci95, walkforward_delta, trust, blocked_by}`. New constants `CANDIDATE_MIN_EFF_N = 10`, `CANDIDATE_TOP_N = 8`, and `_blocked_by(v) -> str`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_strategy_grid.py` (new `CandidateTest` class):

```python
class CandidateTest(unittest.TestCase):
    def _variant(self, vid, dh, dm, eff, survives, trust, p=0.5, wf=1.0):
        return {"variant_id": vid, "side": "NO", "band": [0.10, 0.25], "gate": "mechanical",
                "exit": {"kind": "sl", "amount": 0.15}, "delta_vs_hold": dh,
                "delta_vs_market": dm, "effective_n": eff, "ci95": [0.0, 0.0],
                "walkforward_delta": wf, "p_value": p, "survives_correction": survives,
                "trust": trust}

    def test_positive_near_miss_becomes_a_candidate_with_reason(self):
        full = {"schema_version": sg.SCHEMA_VERSION, "as_of_utc": "2026-07-30T00:00:00+00:00",
                "variants": [
                    # positive both nulls, enough sample, but NOT BH-significant -> candidate
                    self._variant("A", 0.02, 0.01, 40, False, "rejected", p=0.11),
                    # positive both nulls but effective_n < 30 -> candidate, effective_n reason
                    self._variant("B", 0.03, 0.02, 18, False, "insufficient_sample", p=0.5),
                    # NEGATIVE market null -> never a candidate
                    self._variant("C", 0.02, -0.01, 40, False, "rejected", p=0.11),
                    # too small a sample (<10) -> never a candidate
                    self._variant("D", 0.02, 0.01, 5, False, "insufficient_sample", p=0.5)]}
        latest = sg.latest_summary(full)
        ids = [c["variant_id"] for c in latest["candidates"]]
        self.assertEqual(ids, ["B", "A"])   # ranked by delta_vs_market desc (0.02, 0.01)
        by = {c["variant_id"]: c["blocked_by"] for c in latest["candidates"]}
        self.assertIn("effective_n", by["B"])            # 18 < 30
        self.assertIn("not significant", by["A"])        # BH
        self.assertNotIn("C", ids)
        self.assertNotIn("D", ids)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Candidate -v`
Expected: FAIL — `KeyError: 'candidates'`.

- [ ] **Step 3: Implement candidates in `latest_summary`**

In `scripts/research/strategy_grid.py`, add near the other Task-5 constants:

```python
CANDIDATE_MIN_EFF_N = 10
CANDIDATE_TOP_N = 8

def _blocked_by(v):
    """Why a positive-edge variant is NOT 'measured' — precedence: sample, then
    significance, then walk-forward. First shortfall wins."""
    eff = v.get("effective_n") or 0.0
    if eff < SURVIVOR_MIN_EFF_N:
        return "effective_n %.0f < %d" % (eff, SURVIVOR_MIN_EFF_N)
    if not v.get("survives_correction"):
        return "not significant (BH); p=%.3f" % (v.get("p_value") or 1.0)
    return "walk-forward sign flip"
```

In `latest_summary`, after building `out_survivors` and before the `return`, build candidates and include them:

```python
    non_surv = [v for v in full["variants"]
                if v.get("trust") != "measured"
                and (v.get("delta_vs_hold") or 0.0) > 0.0
                and (v.get("delta_vs_market") or 0.0) > 0.0
                and (v.get("effective_n") or 0.0) >= CANDIDATE_MIN_EFF_N]
    non_surv.sort(key=lambda v: v.get("delta_vs_market") or 0.0, reverse=True)
    candidates = [{
        "variant_id": v["variant_id"], "side": v["side"], "band": v["band"],
        "gate": v["gate"], "exit": v["exit"],
        "delta_vs_hold": v["delta_vs_hold"], "delta_vs_market": v["delta_vs_market"],
        "effective_n": v["effective_n"], "ci95": v.get("ci95"),
        "walkforward_delta": v.get("walkforward_delta"),
        "trust": v["trust"], "blocked_by": _blocked_by(v),
    } for v in non_surv[:CANDIDATE_TOP_N]]
```

and add `"candidates": candidates,` to the returned dict (beside `"survivors": out_survivors`).

- [ ] **Step 4: Run test + full module**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -v`
Expected: PASS (all prior + `CandidateTest`).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/tests/test_strategy_grid.py ../scripts/research/strategy_grid.py 2>/dev/null || git add openmarketterminal-qt/tests/test_strategy_grid.py scripts/research/strategy_grid.py
git commit -m "feat(strategy-grid): emit candidates[] (positive near-misses) in -latest.json"
```

---

### Task 2: Pure `KalshiStrategyGridView` core (parse + formatters) + QtTest

**Files:**
- Create: `openmarketterminal-qt/src/services/prediction/kalshi/KalshiStrategyGridView.h`, `.cpp`
- Create/Test: `openmarketterminal-qt/tests/tst_kalshi_strategy_grid_view.cpp`
- Modify: `openmarketterminal-qt/CMakeLists.txt` (add `.cpp` to the service sources beside `Kalshi15mReconcile.cpp`); register the test mirroring an existing `tst_kalshi_*` target.

**Interfaces:**
- Produces (namespace `openmarketterminal::services::prediction::kalshi_ns`):
  - `struct GridRow { QString variant_id, side, band, gate, exit, trust, blocked_by; double delta_vs_hold=0, delta_vs_market=0, effective_n=0; };`
  - `struct GridLatest { bool available=false; int schema_version=0; QString headline; qint64 age_ms=-1; bool stale=false; QVector<GridRow> survivors, candidates; };`
  - `GridLatest parse_grid_latest(const QByteArray& json, qint64 now_ms);`
  - `QString grid_cockpit_line(const GridLatest&);`
  - `QString grid_cli_report(const GridLatest&, bool as_json, const QByteArray& raw);`
  - `inline constexpr qint64 kGridStaleMs = 6LL * 3600LL * 1000LL;`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_kalshi_strategy_grid_view.cpp`:

```cpp
#include <QtTest>
#include "services/prediction/kalshi/KalshiStrategyGridView.h"
namespace k = openmarketterminal::services::prediction::kalshi_ns;

class TstKalshiStrategyGridView : public QObject {
    Q_OBJECT
    static QByteArray sample(const QString& as_of, const QString& body) {
        return QStringLiteral("{\"schema_version\":1,\"as_of_utc\":\"%1\",%2}")
            .arg(as_of, body).toUtf8();
    }
private slots:
    void missing_file_is_unavailable() {
        auto g = k::parse_grid_latest(QByteArray(), 0);
        QVERIFY(!g.available);
        QCOMPARE(k::grid_cockpit_line(g), QStringLiteral("GRID: UNAVAILABLE"));
    }
    void garbage_is_unavailable() {
        QVERIFY(!k::parse_grid_latest(QByteArrayLiteral("not json"), 0).available);
    }
    void no_edge_headline() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[]"),
            QDateTime::fromString("2026-07-30T00:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
        QVERIFY(g.available);
        QCOMPARE(k::grid_cockpit_line(g), QStringLiteral("GRID: no measured edge"));
    }
    void candidate_line_when_no_survivor() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[{\"variant_id\":\"NO|b10-25|mechanical|sl15\","
                   "\"side\":\"NO\",\"band\":[0.1,0.25],\"gate\":\"mechanical\","
                   "\"exit\":{\"kind\":\"sl\",\"amount\":0.15},\"delta_vs_hold\":0.02,"
                   "\"delta_vs_market\":0.008,\"effective_n\":34,\"trust\":\"rejected\","
                   "\"blocked_by\":\"not significant (BH); p=0.110\"}]"),
            QDateTime::fromString("2026-07-30T00:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
        const QString line = k::grid_cockpit_line(g);
        QVERIFY(line.contains("forming"));
        QVERIFY(line.contains("n_eff 34"));
        QVERIFY(line.contains("vs mkt"));
    }
    void stale_is_tagged() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[]"),
            QDateTime::fromString("2026-07-30T00:00:00Z", Qt::ISODate).toMSecsSinceEpoch()
                + 7LL*3600*1000);  // 7h later > 6h bound
        QVERIFY(g.stale);
        QVERIFY(k::grid_cockpit_line(g).contains("STALE"));
    }
};
QTEST_MAIN(TstKalshiStrategyGridView)
#include "tst_kalshi_strategy_grid_view.moc"
```

- [ ] **Step 2: Register + build; verify it fails to compile (missing header)**

Register the test target by mirroring an existing Qt test (find one: `grep -rn "tst_kalshi_15m_reconcile\|tst_kalshi_evidence" openmarketterminal-qt/CMakeLists.txt openmarketterminal-qt/tests/CMakeLists.txt`), linking Qt::Test and `openterminal_core`; add `KalshiStrategyGridView.cpp` to the service source list beside `Kalshi15mReconcile.cpp` (`grep -n Kalshi15mReconcile.cpp openmarketterminal-qt/CMakeLists.txt`).
Run: `cmake --build openmarketterminal-qt/build --target tst_kalshi_strategy_grid_view`
Expected: FAIL — `KalshiStrategyGridView.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/services/prediction/kalshi/KalshiStrategyGridView.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace openmarketterminal::services::prediction::kalshi_ns {

inline constexpr qint64 kGridStaleMs = 6LL * 3600LL * 1000LL;   // 6h

struct GridRow {
    QString variant_id, side, band, gate, exit, trust, blocked_by;
    double delta_vs_hold = 0.0, delta_vs_market = 0.0, effective_n = 0.0;
};
struct GridLatest {
    bool available = false;
    int schema_version = 0;
    QString headline;
    qint64 age_ms = -1;
    bool stale = false;
    QVector<GridRow> survivors, candidates;
};

/// Parse kalshi-strategy-grid-latest.json. Fails CLOSED (available=false) on empty,
/// unparseable, or schema-version-mismatched input. `now_ms` sets age/stale.
GridLatest parse_grid_latest(const QByteArray& json, qint64 now_ms);

/// One advisory line for the bot cockpit.
QString grid_cockpit_line(const GridLatest&);

/// CLI report: headline + survivors + closest candidates (or `raw` when as_json).
QString grid_cli_report(const GridLatest&, bool as_json, const QByteArray& raw);

}  // namespace
```

- [ ] **Step 4: Write the implementation**

Create `KalshiStrategyGridView.cpp` — parse with `QJsonDocument`; on any failure return a default `GridLatest{}` (available=false). Compute `age_ms = now_ms - as_of_ms` (parse `as_of_utc` via `QDateTime::fromString(..., Qt::ISODate)`), `stale = age_ms > kGridStaleMs`. `grid_cockpit_line`: `UNAVAILABLE` when `!available`; else if a survivor exists → `GRID ADVISORY: <side> <band> + <exit> +Xc vs mkt (n_eff N)`; else if a candidate exists → `GRID: forming — <side> <band> +Xc vs mkt, <blocked_by> (n_eff N)`; else `GRID: no measured edge`; append ` (STALE Hh)` when stale. `grid_cli_report`: when `as_json` return `QString::fromUtf8(raw)`; else the headline, a survivors block, and a "closest candidates" block (each with `blocked_by`). Format cents as `+%.1fc` from the dollar deltas ×100.

*(The implementer writes the bodies; the struct fields, function signatures, stale bound, the exact cockpit-line prefixes the test asserts (`GRID: UNAVAILABLE`, `GRID: no measured edge`, `forming`, `n_eff N`, `vs mkt`, `STALE`), and the fail-closed rule above are the contract.)*

- [ ] **Step 5: Build + run the test**

Run: `cmake --build openmarketterminal-qt/build --target tst_kalshi_strategy_grid_view && ctest --test-dir openmarketterminal-qt/build -R tst_kalshi_strategy_grid_view --output-on-failure`
Expected: PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/src/services/prediction/kalshi/KalshiStrategyGridView.* openmarketterminal-qt/tests/tst_kalshi_strategy_grid_view.cpp openmarketterminal-qt/CMakeLists.txt openmarketterminal-qt/tests/CMakeLists.txt
git commit -m "feat(kalshi): pure KalshiStrategyGridView (parse -latest.json + formatters) + QtTest"
```

---

### Task 3: CLI verb `kalshi grid`

**Files:**
- Modify: `src/cli/CommandDispatch.cpp` (add a `grid` branch inside the `topic == "kalshi"` block at ~line 515), reading the evidence file and calling the pure formatter.
- Test: `tests/e2e_kalshi_grid_cli.sh` (mirror an existing `tests/e2e_kalshi_*.sh`).

**Interfaces:**
- Consumes: `kalshi_ns::parse_grid_latest`, `grid_cli_report` (Task 2); `cli::kalshi_evidence_path`.

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e_kalshi_grid_cli.sh` mirroring an existing `tests/e2e_kalshi_*.sh` (find one: `ls tests/e2e_kalshi_*.sh | head`): set `OPENTERMINAL_EVIDENCE_DIR` to a temp dir, write a minimal `kalshi-strategy-grid-latest.json` (`{"schema_version":1,"as_of_utc":"…now…","headline":"no variant beats hold+market after correction","survivors":[],"candidates":[]}`), run `openterminalcli kalshi grid`, assert stdout contains `no measured edge`; run `kalshi grid --json`, assert it emits the raw JSON (contains `"schema_version"`). Register it in `tests/CMakeLists.txt` mirroring a sibling e2e.

- [ ] **Step 2: Run it; verify it fails (unknown subcommand)**

Run: `cd openmarketterminal-qt && bash tests/e2e_kalshi_grid_cli.sh` (after building `OpenMarketTerminal`/CLI).
Expected: FAIL — the CLI prints an unknown-subcommand/usage error for `kalshi grid`.

- [ ] **Step 3: Add the `grid` branch**

Inside `CommandDispatch.cpp`'s `if (topic == "kalshi") { … }` block, mirror the existing `kalshi bot` / `kalshi auto` sub-routing to add:

```cpp
        if (sub == QLatin1String("grid") || sub == QLatin1String("strategy-grid")) {
            const bool as_json = args.contains(QStringLiteral("--json"));
            const QString path = cli::kalshi_evidence_path(
                QStringLiteral("kalshi-strategy-grid-latest.json"));
            QByteArray raw;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) raw = f.readAll();
            const auto g = kalshi_ns::parse_grid_latest(
                raw, QDateTime::currentMSecsSinceEpoch());
            std::fputs(kalshi_ns::grid_cli_report(g, as_json, raw).toUtf8().constData(), stdout);
            std::fputc('\n', stdout);
            return 0;
        }
```

Confirm the exact local variable names (`sub`, `args`) by reading the surrounding `topic == "kalshi"` code, and add the `#include "services/prediction/kalshi/KalshiStrategyGridView.h"` at the top. Add `kalshi grid` to the topic's usage string.

- [ ] **Step 4: Build + run the e2e**

Run: `cmake --build openmarketterminal-qt/build --target OpenMarketTerminal && cd openmarketterminal-qt && bash tests/e2e_kalshi_grid_cli.sh`
Expected: PASS (both the human and `--json` assertions).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/src/cli/CommandDispatch.cpp openmarketterminal-qt/tests/e2e_kalshi_grid_cli.sh openmarketterminal-qt/tests/CMakeLists.txt
git commit -m "feat(cli): kalshi grid — read-only surfacing of the strategy-grid verdict"
```

---

### Task 4: Bot cockpit advisory line

**Files:**
- Modify: `src/screens/kalshi/BotCockpitPresentation.h` (add a `grid_line` field to the scene struct + set it), and `src/screens/kalshi/KalshiBotCockpitView.cpp` (render the line).
- Test: extend the cockpit's presentation test (find it: `grep -rln "present_bot_cockpit\|BotCockpit" tests`).

**Interfaces:**
- Consumes: `kalshi_ns::parse_grid_latest`, `grid_cockpit_line` (Task 2); `cli::kalshi_evidence_path`.

- [ ] **Step 1: Write the failing presentation test**

Find the existing bot-cockpit presentation test and add a case: seed a temp evidence dir with a `kalshi-strategy-grid-latest.json` whose headline is "no measured edge"; build the cockpit scene; assert `scene.grid_line == "GRID: no measured edge"`. Seed a missing file; assert `scene.grid_line == "GRID: UNAVAILABLE"`. (If no presentation test exists for the cockpit, add a small `tst_kalshi_bot_cockpit_grid.cpp` mirroring `tst_kalshi_strategy_grid_view.cpp`'s harness that calls the scene builder directly.)

- [ ] **Step 2: Run it; verify it fails (no grid_line field)**

Run: `cmake --build openmarketterminal-qt/build --target <cockpit test target>`
Expected: FAIL — `grid_line` is not a member of the scene struct.

- [ ] **Step 3: Add the field + populate it**

In `BotCockpitPresentation.h`: add `QString grid_line;` to the cockpit scene struct; in the scene-building function, read the evidence file via `cli::kalshi_evidence_path("kalshi-strategy-grid-latest.json")` and set `scene.grid_line = kalshi_ns::grid_cockpit_line(kalshi_ns::parse_grid_latest(raw, now_ms));` (mirror how the KPI strip reads `kalshi-bot-gate.json`). Add the include.

- [ ] **Step 4: Render it in the view**

In `KalshiBotCockpitView.cpp`, render `scene.grid_line` as one advisory-labelled line in the cockpit layout (mirror an existing single-line label render, e.g. the mood/KPI line). Keep it visually a distinct ADVISORY line, beside — not part of — the gate KPIs.

- [ ] **Step 5: Build + run tests**

Run: `cmake --build openmarketterminal-qt/build --target OpenMarketTerminal && cmake --build openmarketterminal-qt/build --target <cockpit test target> && ctest --test-dir openmarketterminal-qt/build -R <cockpit test> --output-on-failure`
Expected: app builds; cockpit test PASS.

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/src/screens/kalshi/BotCockpitPresentation.h openmarketterminal-qt/src/screens/kalshi/KalshiBotCockpitView.cpp <cockpit test file>
git commit -m "feat(kalshi): advisory strategy-grid line in the bot cockpit"
```

---

## Self-Review

**Spec coverage:** `candidates[]` emission with the pre-registered rule + `blocked_by` precedence (Task 1) ✓; pure `KalshiStrategyGridView` parse + both formatters, fail-closed + stale (Task 2) ✓; CLI verb read-only + `--json` (Task 3) ✓; bot cockpit advisory line, UNAVAILABLE/STALE fail-closed (Task 4) ✓; no recompute/gate-coupling, humility metadata on every row (constraints + Tasks 1–2) ✓. **Deferred (separate):** the maker quote-lag engine.

**Placeholder scan:** Tasks 1–2 carry complete literal code + tests. Tasks 2 (`.cpp` body), 3, 4 describe bodies/insertion points that must be grep-and-mirrored against live code (CLI local var names, the cockpit scene struct, the view's line render) — these are concrete grep-named steps, not vague instructions, and every asserted output string/field the tests check is given exactly.

**Type consistency:** `GridRow`/`GridLatest` fields are identical between Task 2's header, its test, and the consumers; `parse_grid_latest`/`grid_cockpit_line`/`grid_cli_report` signatures match across Tasks 2–4; the `candidates[]` item keys emitted in Task 1 are exactly the fields Task 2's `parse_grid_latest` reads; `kGridStaleMs = 6h` matches `GRID_STALE_MS` in the spec.
