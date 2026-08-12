# Ablation matrix — what do the non-market features contribute?

**Analysis only.** Nothing here is read by the bot, the gate, or any admission path.
No production, admission, sizing or sealed-parameter change accompanies this measurement.

Snapshot taken 2026-08-12 07:16. Regenerate with `python3 scripts/kalshi_advise/ablation_matrix.py`.

## Result

**No model beats `market_raw`.** On every family and every preregistered slice, the raw
market mid scores at least as well as every fitted alternative. Where a difference is
statistically established, it is always in the market's favour.

## Method

- Walk-forward in settlement order, each contract scored BEFORE it trains the model.
- Identical folds and identical test rows for all six models, so the paired bootstrap is valid.
- Slices are **preregistered** and defined on the market price or on `z` only — never on a
  candidate model's own output. The production `bet_eligible` slice is defined by
  `|p - mid| >= 0.10`, i.e. the model's own disagreement, which gives every candidate a
  different test population; this analysis must not repeat that.
- `delta` is model minus `market_raw`, so **positive is worse**. `SIG` marks an interval
  excluding zero.

### Limitation

`resolved_record` carries no timestamp. Order is the record's append order, which is
settlement order. That assumption is load-bearing for the walk-forward and is stated
rather than hidden.

### Slices

- `all` — every settled contract
- `eval_band` — 0.10 <= market mid <= 0.90 (fixed, preregistered)
- `extreme_z` — |z| >= 2.0
- `favorites_gt70` — market mid > 0.70
- `longshots_lt30` — market mid < 0.30
- `near_boundary` — |z| < 0.5

## KXGOLD15M — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXGOLDD — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXGOLDH — 209 settled contracts

### all (n=209)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.1039 | 0.3189 | 15.1 | — |
| `market_logit` | 0.1440 | 0.4950 | 16.4 | +0.0402 [+0.0290, +0.0509] **SIG** |
| `physics_no_market` | 0.1443 | 0.4541 | 11.1 | +0.0405 [+0.0244, +0.0574] **SIG** |
| `market_physics` | 0.1284 | 0.4339 | 13.4 | +0.0245 [+0.0109, +0.0385] **SIG** |
| `market_physics_signals` | 0.1284 | 0.4339 | 13.4 | +0.0245 [+0.0109, +0.0385] **SIG** |
| `signals_only` | 0.2017 | 0.5941 | 11.5 | +0.0978 [+0.0792, +0.1168] **SIG** |

### eval_band (n=96)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.2232 | 0.6405 | 21.7 | — |
| `market_logit` | 0.2276 | 0.7174 | 22.7 | +0.0043 [-0.0154, +0.0239] |
| `physics_no_market` | 0.2479 | 0.6945 | 28.2 | +0.0246 [-0.0063, +0.0561] |
| `market_physics` | 0.2241 | 0.6897 | 28.0 | +0.0009 [-0.0226, +0.0269] |
| `market_physics_signals` | 0.2241 | 0.6897 | 28.0 | +0.0009 [-0.0226, +0.0269] |
| `signals_only` | 0.3120 | 0.8293 | 26.9 | +0.0887 [+0.0510, +0.1275] **SIG** |

### favorites_gt70 (n=14)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0374 | 0.2016 | 19.2 | — |
| `market_logit` | 0.0940 | 0.3621 | 30.6 | +0.0566 [+0.0371, +0.0854] **SIG** |
| `physics_no_market` | 0.2183 | 0.6430 | 46.7 | +0.1810 [+0.1252, +0.2700] **SIG** |
| `market_physics` | 0.0852 | 0.3260 | 29.1 | +0.0478 [+0.0004, +0.1369] **SIG** |
| `market_physics_signals` | 0.0852 | 0.3260 | 29.1 | +0.0478 [+0.0004, +0.1369] **SIG** |
| `signals_only` | 0.4740 | 1.1668 | 68.8 | +0.4366 [+0.4261, +0.4466] **SIG** |

### longshots_lt30 (n=165)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0785 | 0.2515 | 10.2 | — |
| `market_logit` | 0.1218 | 0.4139 | 17.4 | +0.0432 [+0.0312, +0.0547] **SIG** |
| `physics_no_market` | 0.1012 | 0.3529 | 11.8 | +0.0226 [+0.0098, +0.0352] **SIG** |
| `market_physics` | 0.0982 | 0.3377 | 12.2 | +0.0196 [+0.0091, +0.0302] **SIG** |
| `market_physics_signals` | 0.0982 | 0.3377 | 12.2 | +0.0196 [+0.0091, +0.0302] **SIG** |
| `signals_only` | 0.1437 | 0.4710 | 22.7 | +0.0652 [+0.0520, +0.0778] **SIG** |

### near_boundary (n=31)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.2747 | 0.7438 | 37.9 | — |
| `market_logit` | 0.2899 | 0.9946 | 43.3 | +0.0151 [-0.0223, +0.0587] |
| `physics_no_market` | 0.3594 | 0.9543 | 48.6 | +0.0847 [+0.0087, +0.1555] **SIG** |
| `market_physics` | 0.3215 | 1.0269 | 48.9 | +0.0468 [-0.0162, +0.1099] |
| `market_physics_signals` | 0.3215 | 1.0269 | 48.9 | +0.0468 [-0.0162, +0.1099] |
| `signals_only` | 0.4073 | 1.0330 | 48.0 | +0.1325 [+0.0612, +0.2002] **SIG** |

### extreme_z (n=86)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0019 | 0.0403 | 4.0 | — |
| `market_logit` | 0.0740 | 0.3064 | 25.7 | +0.0721 [+0.0613, +0.0842] **SIG** |
| `physics_no_market` | 0.0574 | 0.2487 | 21.4 | +0.0555 [+0.0394, +0.0768] **SIG** |
| `market_physics` | 0.0506 | 0.2221 | 19.6 | +0.0487 [+0.0333, +0.0685] **SIG** |
| `market_physics_signals` | 0.0506 | 0.2221 | 19.6 | +0.0487 [+0.0333, +0.0685] **SIG** |
| `signals_only` | 0.1080 | 0.3946 | 31.1 | +0.1062 [+0.0969, +0.1178] **SIG** |

## KXSILVER15M — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXSILVERD — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXSILVERH — 211 settled contracts

### all (n=211)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0747 | 0.2574 | 13.9 | — |
| `market_logit` | 0.1566 | 0.4948 | 17.4 | +0.0819 [+0.0679, +0.0965] **SIG** |
| `physics_no_market` | 0.1345 | 0.4323 | 13.8 | +0.0598 [+0.0423, +0.0778] **SIG** |
| `market_physics` | 0.1123 | 0.3800 | 15.9 | +0.0376 [+0.0209, +0.0550] **SIG** |
| `market_physics_signals` | 0.1123 | 0.3800 | 15.9 | +0.0376 [+0.0209, +0.0550] **SIG** |
| `signals_only` | 0.1993 | 0.5891 | 11.9 | +0.1246 [+0.1084, +0.1411] **SIG** |

### eval_band (n=110)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.1407 | 0.4489 | 18.7 | — |
| `market_logit` | 0.1992 | 0.5874 | 26.9 | +0.0585 [+0.0378, +0.0813] **SIG** |
| `physics_no_market` | 0.1915 | 0.5641 | 21.7 | +0.0508 [+0.0233, +0.0812] **SIG** |
| `market_physics` | 0.1586 | 0.4940 | 21.7 | +0.0179 [-0.0088, +0.0462] |
| `market_physics_signals` | 0.1586 | 0.4940 | 21.7 | +0.0179 [-0.0088, +0.0462] |
| `signals_only` | 0.2676 | 0.7332 | 14.2 | +0.1269 [+0.0984, +0.1571] **SIG** |

### favorites_gt70 (n=18)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0405 | 0.2099 | 20.0 | — |
| `market_logit` | 0.2973 | 0.8150 | 54.5 | +0.2569 [+0.1724, +0.3522] **SIG** |
| `physics_no_market` | 0.3157 | 0.8551 | 56.1 | +0.2752 [+0.1763, +0.3797] **SIG** |
| `market_physics` | 0.2493 | 0.7312 | 49.9 | +0.2088 [+0.0978, +0.3347] **SIG** |
| `market_physics_signals` | 0.2493 | 0.7312 | 49.9 | +0.2088 [+0.0978, +0.3347] **SIG** |
| `signals_only` | 0.4357 | 1.0822 | 66.0 | +0.3953 [+0.3784, +0.4129] **SIG** |

### longshots_lt30 (n=157)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0445 | 0.1755 | 5.0 | — |
| `market_logit` | 0.1142 | 0.4032 | 25.7 | +0.0697 [+0.0584, +0.0803] **SIG** |
| `physics_no_market` | 0.0779 | 0.3034 | 19.7 | +0.0333 [+0.0216, +0.0444] **SIG** |
| `market_physics` | 0.0710 | 0.2808 | 18.1 | +0.0265 [+0.0154, +0.0374] **SIG** |
| `market_physics_signals` | 0.0710 | 0.2808 | 18.1 | +0.0265 [+0.0154, +0.0374] **SIG** |
| `signals_only` | 0.1275 | 0.4376 | 28.3 | +0.0830 [+0.0721, +0.0929] **SIG** |

### near_boundary (n=36)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.2332 | 0.6601 | 33.0 | — |
| `market_logit` | 0.2858 | 0.7666 | 40.7 | +0.0527 [+0.0214, +0.0868] **SIG** |
| `physics_no_market` | 0.3009 | 0.8031 | 41.7 | +0.0677 [+0.0120, +0.1271] **SIG** |
| `market_physics` | 0.2306 | 0.6520 | 35.7 | -0.0025 [-0.0523, +0.0527] |
| `market_physics_signals` | 0.2306 | 0.6520 | 35.7 | -0.0025 [-0.0523, +0.0527] |
| `signals_only` | 0.4128 | 1.0414 | 51.6 | +0.1796 [+0.1271, +0.2308] **SIG** |

### extreme_z (n=66)

| model | Brier | log loss | calib (pts) | vs market_raw [95% CI] |
|---|---|---|---|---|
| `market_raw` | 0.0060 | 0.0638 | 7.5 | — |
| `market_logit` | 0.1236 | 0.4253 | 21.6 | +0.1176 [+0.0896, +0.1512] **SIG** |
| `physics_no_market` | 0.0804 | 0.2949 | 13.7 | +0.0744 [+0.0433, +0.1128] **SIG** |
| `market_physics` | 0.0742 | 0.2810 | 12.7 | +0.0682 [+0.0358, +0.1090] **SIG** |
| `market_physics_signals` | 0.0742 | 0.2810 | 12.7 | +0.0682 [+0.0358, +0.1090] **SIG** |
| `signals_only` | 0.1328 | 0.4475 | 24.6 | +0.1268 [+0.1087, +0.1476] **SIG** |

## KXWTI — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXWTI15M — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## KXWTIH — SKIPPED (only 0 resolved contracts)

Reported as skipped rather than omitted: *too few to measure* and *not measured* must
not look identical. Every family restarted at zero after the per-family splits.

## What this does and does not establish

Established:

- `market_logit` — a logistic fitted on the mid ALONE — is significantly **worse** than the
  raw mid on both families. The learner cannot reproduce the baseline it is handed.
- `market_physics` and `market_physics_signals` are identical to four decimals everywhere,
  because `ENSEMBLE_FEATURES` are 0.0 stubs. Those feeds contribute nothing.
- `signals_only` is the worst model on every slice — it predicts a constant.
- In `eval_band` the full model is **statistically indistinguishable** from the market
  (interval contains zero); everywhere else it is significantly worse.
- At `extreme_z` the market is nearly perfect (Brier 0.0019, calibration error 4.0 points)
  and every model degrades it.

NOT established:

- That an offset architecture `logit(p) = logit(p_market) + g(x)` would improve predictions.
  It would only guarantee faithful reproduction of the market when `g = 0`. Whether `g`
  earns its place is a separate experiment with its own out-of-sample evidence.
- Anything about families with no settled evidence. They are skipped, not judged.
- Favorites slices are n=14 and n=18; those intervals are wide even where they exclude zero.
