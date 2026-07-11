# Kalshi Strategy → Open-Terminal Prediction Window — Integration Design

**Date:** 2026-07-08
**Status:** Design (for later) — pending the hard precondition below
**Scope:** Surface the validated Kalshi trading strategy (edge/ladder-pricing/
sizing/risk/exit/attribution/research) *inside* Open-Terminal's prediction
window, reusing the terminal's existing Kalshi client, secure credential store,
DOM-ladder widget, live spot feeds, and hardened trading gates. The strategy
logic currently lives as a standalone, demo-only Python project at
`~/src/kalshi-pipeline` (its own repo) — this doc is how it becomes a native
feature of the terminal.

---

## Hard precondition (do not skip)

**The standalone pipeline must pass its own demo paper-trial bar BEFORE any
integration work starts:** ≥40 resolved paper positions, Brier score
meaningfully better than quote-the-market, and positive simulated P&L after the
Kalshi fee curve + slippage (the trial criteria already defined in
`kalshi-pipeline/DESIGN.md`). Wiring an *unvalidated* trading strategy into a
shipping desktop product — one whose release is already fragile (the open
Windows MSVC ICE) — adds real-money blast radius for no proven edge. Validate
standalone first; integrate second. This doc assumes that gate has passed.

Blocking today: the pipeline needs **Kalshi demo API keys** to run the trial
(the reused SimplyMoney key is production-only and 401s on demo).

---

## The complementary split (why this is a short hop, not a rewrite)

The terminal already owns the *hard half*; the pipeline owns the *missing half*.

| Layer | Terminal already has (reuse) | Pipeline has (port / connect) |
|-------|------------------------------|-------------------------------|
| Kalshi data | `KalshiRestClient`, `KalshiWsClient`, `KalshiAdapter` (~2,452 lines, REST+WS+auth) | `kalshi_client.py` (REST only, no WS) → **drop, reuse native** |
| Credentials | `PredictionCredentialStore` (keychain-backed), `KalshiCredentials` | plaintext `config.json` + loose `.pem` → **drop, reuse secure store** |
| Spot feeds | live crypto spot (used by the crypto screens) | Binance/Coinbase/OKX connectors → **reuse native feeds for realized vol** |
| UI | `CryptoLadder`/`CryptoLadderModel` DOM-ladder widget; Predictions screen (`src/screens/polymarket`) | none (headless) → **render ladder + fair-value overlay** |
| Execution safety | `allow_trading` / `live_trading_armed` / `kill_switch`, paper-vs-live gates | demo-hardwired URL → **route live orders through the gates** |
| LLM | `LlmService` / `AgentService` | `research.py` (Claude API, anchoring-firewalled) → **reuse LlmService or keep as bundled helper** |
| **Strategy brain** | **— (this is what the terminal lacks) —** | `edge` (fee curve), `ladder` (fair value + rules), `sizing` (Kelly), `risk` (gates), `manage` (exit + inventory), `review` (Brier + attribution), `run` (orchestration) |

So the strategy math is the only genuinely new code; everything it depends on
(data, creds, UI, execution, LLM) already exists natively.

---

## Architecture

```
 Kalshi WS (native KalshiWsClient) ─┐
 Kalshi REST (native KalshiRestClient) ├─▶ KalshiStrategyService (new, C++) ─┐
 live spot feeds (native) ──────────────┘     │  ladder fair-value + edge      │
                                               │  sizing + risk gates           ▼
 PredictionCredentialStore (native, secure) ──▶│  exit manager (watch loop)   Predictions screen
 LlmService (native, event research) ─────────▶│  attribution / journal        │  • CryptoLadder + fair overlay
                                               └───────────────┬───────────────┘  • candidates panel
                                                               │                   • positions/exits panel
                                       paper: journal only     │                   • attribution panel
                                       live:  gated order ─────┴─▶ allow_trading / armed / kill_switch
                                                                    → KalshiRestClient.place_order
```

### Recommended approach: native C++ port of the pure math, connect to existing infra

The strategy's pure, deterministic math is *small* and belongs in-process (in
the trade path and the UI), so port it to native C++ (it mirrors the Python
1:1 and its unit tests port directly). The heavy/LLM piece stays out of the hot
path. Concretely:

- **Port to native C++** (`src/services/prediction/kalshi_strategy/`):
  - `KalshiEdgeModel` ← `edge.py` — Kalshi taker-fee curve `ceil(0.07·C·P·(1−P))`,
    net edge in payout units, side selection.
  - `KalshiLadderModel` ← `ladder.py` — survival-function fair value
    `P(close≥K)=Φ((spot−K)/(spot·σ))`, the **ignore-pinned-extremes (≤1¢/≥99¢)**
    rule, the **lower favorites edge bar** (`ladder.min_edge`), and
    `monotonicity_breaks()`.
  - `PositionSizer` ← `sizing.py` — quarter-Kelly clamped to the per-trade cap.
  - `KalshiRiskGate` ← `risk.py` — or reuse the terminal's existing trading
    caps if they subsume it.
  - `KalshiExitManager` ← `manage.py` — the **mandatory-stop-not-gated-on-a-bid**
    fix (max-loss / max-hold / force-before-close on achievable liquidation
    value; escalate to market order), + the correlation/inventory gate.
  - `KalshiTrialReview` ← `review.py` — Brier vs market + PnL **attribution**
    (minute-of-hour bucket, prob lane, YES/NO, threshold-vs-range market kind).
  - `KalshiStrategyService` ← `run.py` — orchestrator + `watch` exit loop, on
    the terminal's event loop.
- **Reuse native** (no new code): `KalshiRestClient`/`KalshiWsClient`,
  `PredictionCredentialStore`, spot feeds, the trading gates, `CryptoLadder`.
- **Event-market research** (`research.py`): reuse `LlmService`/`AgentService`
  with the same anchoring-firewall prompt (research never sees price; estimate
  is journaled before the book is read), OR keep `research.py` as a bundled
  Python helper the terminal invokes (the terminal already runs bundled Python).
  The crypto-ladder path needs **no** LLM — it's the vol/quant model.

Rationale for not shipping the Python subprocess as the primary path: the
terminal already has the Kalshi data + creds natively, so a Python subprocess
would *re-add* the plaintext-key + duplicate-client problems integration is
meant to remove. Keep Python only for the LLM research helper (if not using
`LlmService`), where it's off the trade path.

---

## UI: the payoff feature

Extend the Predictions screen with a Kalshi hourly-crypto panel:
- **Strike-ladder view** — reuse `CryptoLadder`/`CryptoLadderModel` to render the
  live "$X or above" ladder, overlaid with the model's **fair value per strike**
  (market vs fair, color-coded), so mispriced ATM strikes and any monotonicity
  break are *visible* — the thing a headless log can't show.
- **Candidates panel** — the tradable strikes the scan surfaced, with net edge,
  side, fair vs market, and why (or why a strike was skipped: pinned extreme /
  below bar / range-kind filtered).
- **Positions + exits panel** — open positions with the exit manager's live
  decision (HOLD / CASHOUT / STOP + escalate), driven by the `watch` loop.
- **Attribution panel** — the PnL breakdown by minute-bucket / lane / side /
  market-kind (the "learn where the edge is" table), read from the journal.

---

## Safety model (carried over intact)

- **Demo by default.** The credential store selects demo vs prod; the strategy
  service defaults to demo/paper and never auto-arms.
- **Live is a deliberate, gated action.** Any real order routes through the
  existing `allow_trading` / `live_trading_armed` / `kill_switch` gates and
  starts with extreme caps (mirroring the pipeline's `--live --once` first
  order). No "flip a config flag to go live."
- **The strategy's own guardrails survive the port:** ignore pinned 1¢/99¢
  strikes; threshold-only market-kind by default (range is harder / lost early);
  fee-aware edge gate; the exit manager's mandatory stops; correlation cap on
  concurrent same-hour positions.

---

## Phasing

- **Phase 0 (precondition):** standalone demo trial passes its bar. *(blocked on
  demo keys)*
- **Phase 1 — native strategy math + tests.** Port `KalshiEdgeModel`,
  `KalshiLadderModel`, `PositionSizer`, `KalshiExitManager`, `KalshiTrialReview`;
  port the 26 pipeline unit tests to QtTest. No UI, no live data — pure logic,
  neuter-verified. Ship-able/reviewable on its own.
- **Phase 2 — service wiring (paper).** `KalshiStrategyService` consumes the
  native Kalshi WS/REST + spot feeds + credential store; runs the discover →
  ladder-scan → edge → size → risk → (paper) journal → exit-watch loop; writes
  the journal + attribution. Paper only.
- **Phase 3 — UI.** Predictions-screen panels above (ladder fair overlay,
  candidates, positions/exits, attribution).
- **Phase 4 — live behind gates.** Real orders through the trading gates, extreme
  caps first, kill-switch verified; live only after Phase 0's paper edge holds.

---

## Files (terminal side)

- Create: `src/services/prediction/kalshi_strategy/{KalshiEdgeModel,KalshiLadderModel,PositionSizer,KalshiExitManager,KalshiTrialReview,KalshiStrategyService}.{h,cpp}`
- Create: `tests/tst_kalshi_strategy.cpp` (port of `kalshi-pipeline/test_pipeline.py`, register in `tests/CMakeLists.txt`)
- Modify: the Predictions screen (`src/screens/polymarket/…` or a new
  `src/screens/predictions/…`) — add the ladder/candidates/positions/attribution
  panels; reuse `CryptoLadder`.
- Reuse (no change): `services/prediction/kalshi/*`, `PredictionCredentialStore`,
  the crypto spot feeds, the trading gates, `LlmService`.
- Modify: `src/CMakeLists.txt` (compile the new service + panels).

## Out of scope

- Re-implementing the Kalshi REST/WS client or auth (the terminal has it).
- Putting the LLM in the trade path (research is off-path, event-markets only).
- Integrating before the standalone demo trial passes.
- Range-market pricing (threshold-only to start; range is a later addition).

## Reference

The validated behavior + exact math live in the standalone
`~/src/kalshi-pipeline` (its own repo): `edge.py`, `ladder.py`, `sizing.py`,
`risk.py`, `manage.py`, `review.py`, `discover.py`, `research.py`, `run.py`, with
`test_pipeline.py` (26 tests) as the executable spec each native module ports.
