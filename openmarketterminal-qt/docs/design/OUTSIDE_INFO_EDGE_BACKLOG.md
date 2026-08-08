# Outside-info edge backlog

Kalshi mid already prices most of the Gaussian race. Autopsy lesson: do not
retune `spot_calibrator` / race vol for profit. Edge, if any, comes from
**faster, settlement-aligned, or orthogonal** information.

Promotion ladder (all families):

1. Observe the feature (logged, no bid change)
2. Score ablation vs raw mid on the family scoreboard
3. Use as **veto / confirm** only
4. Tiny probability tilt (only after veto/confirm wins for ≥2 weeks)
5. Trust flip at `n ≥ 100` scored contracts and `adds_value_over_market`

Hard rules:

- No trust leak across `threshold` / `kxbtc15m` / `commodities15m`
- Paper pause ≠ kill switch; AI cannot arm live
- Prefer Kalshi recorded YES/NO at settle; refuse on feed holes
- Do not claim Yahoo ≡ Pyth — measure settlement parity

## Idea inventory

| # | Idea | Family | Status |
|---|------|--------|--------|
| 1 | Pyth true-settle feed (GOLD/SILVER/WTI) | commodities15m | Phase 1 shipped |
| 2 | Crypto venue lead vs sticky Kalshi mid (lag windows) | kxbtc15m | Phase 1 shipped |
| 3 | BRTI 60s-average predictor (match payout) | kxbtc15m | Phase 2 shipped |
| 4 | Commodity futures tape into candle close | commodities15m | Phase 3 shipped |
| 5 | Session / event vol-regime priors | both | Phase 3 shipped |
| 6 | Tiny probability tilts after veto/confirm wins | both | Phase 4 |

## Phase 1 feed map (Pyth Hermes Core)

| Series | Hermes symbol | Feed id |
|--------|---------------|---------|
| KXGOLD15M | Metal.XAU/USD | `765d2ba906dbc32ca17cc11f5310a89e9ee1f6420508c63861f2f8ba4ee34bb2` |
| KXSILVER15M | Metal.XAG/USD | `f2fb02c32b055c805e7238d628e5e9dadef274376114eb1f012337cabe93871e` |
| KXWTI15M | Metal.XTI/USD | `a35b407f0fa4b027c2dfa8dff0b7b99b853fb4d326a9e9906271933237b90c1c` |

Endpoint: `https://hermes.pyth.network/v2/updates/price/latest?ids[]=…`

Yahoo (`GC=F` / `SI=F` / `CL=F`) remains cold fallback when Pyth is down.

## Phase 1 lag windows (BTC)

Daemon already tracks independent venue spot vs YES mid over 30s. Persist:

- `venue_lead_bps_30s`
- `mid_lag_cents_30s`
- `lead_confirms_direction`
- `lead_conflicts`

Calibrator ablations (scored beside physics + mid):

- `physics` — current Gaussian
- `physics_veto_on_conflict` — clamp to mid when venue lead conflicts with mid move
- `physics_confirm_only` — leave mid only when venue lead confirms

## Phase 2 — BRTI 60s average (shipped)

- Daemon parses `avg_60s_data` from CF Benchmarks BRTI WS payload
- Horizon / underlying expose `brti_avg_60s`
- Calibrator ablation `physics_brti_avg60` scores P(close>open) on avg60
  (settlement underlier) beside lag ablations; trust still fail-closed at n≥100

## Phase 3 — Futures tape + session/vol priors (shipped)

Shared helpers: `scripts/kalshi_advise/outside_info_features.py`

Commodities ablations (schema 3):
- `physics_tape_confirm_near_close` — Yahoo futures tape must confirm race
  direction in the final 3 minutes; else clamp to mid
- `physics_vol_regime_confirm` — quiet / weekend clamps to mid

KXBTC15M ablation (schema 4):
- `physics_vol_regime_confirm` — same quiet/weekend clamp beside lag/BRTI

## Later phases

- **Phase 4:** Tiny tilts only after veto/confirm wins ablation for 2+ weeks
  (currently blocked: kxbtc15m `adds_value_over_market` is still false)

## Measurement (do not skip)

Paper decide now uses venue-lead / BRTI-avg60 as **fade-ban confirm only**
(not a tilt). After deploy, score settlements with:

`kalshi bot postmortem --post-gate`

Watch: fade lifts, early_exit cashouts, `cheap_no_crushed_by_yes`, favourite
cross losses, mean loss vs pre-gate ≈ −$1.79. Separately keep scoring
`physics_confirm_only` / `physics_veto_on_conflict` / `physics_brti_avg60`
vs mid on a rolling window — Phase 4 needs weeks of wins, not a retune.

## Non-goals

- Merge calibrator trust across families
- Retune hourly threshold calibrator for 15m profit
- Weaken human-arm / kill-switch / hard caps
- Treat Yahoo as Pyth without a measured parity rate
