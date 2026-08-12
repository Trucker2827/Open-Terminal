# Offset architecture + event-clustered bootstrap

**ANALYSIS ONLY.** No production, sealed-parameter, admission, sizing or authorization change.

Snapshot 2026-08-12 10:16. Regenerate: `python3 scripts/kalshi_advise/offset_model.py`

## Verdict: no family x horizon qualifies

The offset architecture is **safer** than the unconstrained form, but **no predictive model
has earned production authority**.

## Sign convention (frozen)

`delta = Brier(baseline) - Brier(model)`. **POSITIVE means the model IMPROVED.**
Promotion requires the ENTIRE interval above zero. Pinned by a test; a rule written in one
sign and computed in the other is a contradiction that survives every review.

## The event-cluster bootstrap supersedes #250

#250's intervals resampled **contracts**. Contracts within one settlement event are
different strikes on a SINGLE price path -- one draw's worth of information shared between
them -- so treating them as independent produced intervals several times too narrow.
**Those results are superseded by this document.**

Cluster identifier: `(round(spot,2), round(sqrt_minutes_left,3))` -- contracts observed at
the same spot and the same time-to-close. `resolved_record` carries no event id, so this is
a documented proxy, deliberately conservative: merging two genuinely distinct events only
widens the interval.

Measured collapse in independent evidence:

| family | contracts | independent events |
|---|---|---|
| KXGOLDH | 264 | **81** |
| KXSILVERH | 264 | **45** |
| KXWTIH | 55 | **7** |

## Every comparison

### KXGOLD15M — SKIPPED (only 0 resolved contracts)

### KXGOLDD — SKIPPED (only 0 resolved contracts)

### KXGOLDH — 264 contracts

`g` moved the market on **53%** of contracts; median |shift| 1.25 pts, max 15.3 pts.

| slice | n | events | model | delta vs market_raw [95% CI] | vs market_physics |
|---|---|---|---|---|---|
| all | 264 | 81 | `market_physics` | -0.0190 [-0.0498, +0.0116] | — |
| all | 264 | 81 | `offset` | +0.0031 [-0.0051, +0.0114] | +0.0221 |
| all | 264 | 81 | `offset_do_no_harm` | +0.0041 [-0.0037, +0.0118] | +0.0230 |
| eval_band | 140 | 44 | `market_physics` | +0.0014 [-0.0453, +0.0448] | — |
| eval_band | 140 | 44 | `offset` | +0.0055 [-0.0108, +0.0207] | +0.0041 |
| eval_band | 140 | 44 | `offset_do_no_harm` | +0.0073 [-0.0078, +0.0207] | +0.0059 |
| favorites_gt70 | 16 | 6 | `market_physics` | -0.0337 [-0.1751, +0.0219] | — |
| favorites_gt70 | 16 | 6 | `offset` | +0.0142 [+0.0074, +0.0255] **QUALIFIES** | +0.0479* |
| favorites_gt70 | 16 | 6 | `offset_do_no_harm` | +0.0143 [+0.0077, +0.0255] **QUALIFIES** | +0.0480* |
| longshots_lt30 | 198 | 67 | `market_physics` | -0.0235 [-0.0426, -0.0046] | — |
| longshots_lt30 | 198 | 67 | `offset` | -0.0005 [-0.0035, +0.0027] | +0.0231* |
| longshots_lt30 | 198 | 67 | `offset_do_no_harm` | -0.0000 [-0.0031, +0.0029] | +0.0235* |
| near_boundary | 44 | 16 | `market_physics` | +0.0078 [-0.0885, +0.0881] | — |
| near_boundary | 44 | 16 | `offset` | +0.0061 [-0.0344, +0.0418] | -0.0017 |
| near_boundary | 44 | 16 | `offset_do_no_harm` | +0.0139 [-0.0210, +0.0420] | +0.0061 |
| extreme_z | 103 | 43 | `market_physics` | -0.0410 [-0.0734, -0.0155] | — |
| extreme_z | 103 | 43 | `offset` | +0.0000 [-0.0005, +0.0004] | +0.0410* |
| extreme_z | 103 | 43 | `offset_do_no_harm` | +0.0000 [-0.0005, +0.0004] | +0.0410* |

### KXSILVER15M — SKIPPED (only 0 resolved contracts)

### KXSILVERD — SKIPPED (only 0 resolved contracts)

### KXSILVERH — 264 contracts

`g` moved the market on **62%** of contracts; median |shift| 1.60 pts, max 18.6 pts.

| slice | n | events | model | delta vs market_raw [95% CI] | vs market_physics |
|---|---|---|---|---|---|
| all | 264 | 45 | `market_physics` | -0.0368 [-0.0690, -0.0095] | — |
| all | 264 | 45 | `offset` | +0.0007 [-0.0050, +0.0060] | +0.0375* |
| all | 264 | 45 | `offset_do_no_harm` | +0.0004 [-0.0055, +0.0058] | +0.0372* |
| eval_band | 135 | 18 | `market_physics` | -0.0257 [-0.0687, +0.0168] | — |
| eval_band | 135 | 18 | `offset` | +0.0006 [-0.0108, +0.0109] | +0.0262 |
| eval_band | 135 | 18 | `offset_do_no_harm` | -0.0000 [-0.0117, +0.0104] | +0.0256 |
| favorites_gt70 | 18 | 5 | `market_physics` | -0.2088 [-0.4033, -0.0014] | — |
| favorites_gt70 | 18 | 5 | `offset` | -0.0068 [-0.0298, +0.0184] | +0.2020* |
| favorites_gt70 | 18 | 5 | `offset_do_no_harm` | +0.0007 [-0.0232, +0.0186] | +0.2095* |
| longshots_lt30 | 207 | 44 | `market_physics` | -0.0278 [-0.0453, -0.0108] | — |
| longshots_lt30 | 207 | 44 | `offset` | +0.0023 [-0.0010, +0.0053] | +0.0301* |
| longshots_lt30 | 207 | 44 | `offset_do_no_harm` | +0.0019 [-0.0013, +0.0047] | +0.0297* |
| near_boundary | 36 | 6 | `market_physics` | +0.0025 [-0.0983, +0.0897] | — |
| near_boundary | 36 | 6 | `offset` | -0.0030 [-0.0357, +0.0257] | -0.0056 |
| near_boundary | 36 | 6 | `offset_do_no_harm` | -0.0030 [-0.0357, +0.0257] | -0.0056 |
| extreme_z | 93 | 26 | `market_physics` | -0.0504 [-0.1179, -0.0125] | — |
| extreme_z | 93 | 26 | `offset` | +0.0009 [-0.0021, +0.0031] | +0.0513* |
| extreme_z | 93 | 26 | `offset_do_no_harm` | +0.0018 [+0.0006, +0.0033] **QUALIFIES** | +0.0523* |

### KXWTI — SKIPPED (only 0 resolved contracts)

### KXWTI15M — SKIPPED (only 0 resolved contracts)

### KXWTIH — 55 contracts

`g` moved the market on **67%** of contracts; median |shift| 1.92 pts, max 16.9 pts.

| slice | n | events | model | delta vs market_raw [95% CI] | vs market_physics |
|---|---|---|---|---|---|
| all | 55 | 7 | `market_physics` | -0.0573 [-0.1186, +0.0023] | — |
| all | 55 | 7 | `offset` | +0.0090 [-0.0006, +0.0232] | +0.0663* |
| all | 55 | 7 | `offset_do_no_harm` | +0.0003 [+0.0000, +0.0006] | +0.0576 |
| eval_band | 23 | 4 | `market_physics` | +0.0456 [-0.0694, +0.1341] | — |
| eval_band | 23 | 4 | `offset` | +0.0234 [+0.0080, +0.0429] **QUALIFIES** | -0.0222 |
| eval_band | 23 | 4 | `offset_do_no_harm` | +0.0001 [+0.0000, +0.0006] | -0.0455 |
| longshots_lt30 | 46 | 6 | `market_physics` | -0.0972 [-0.1826, -0.0716] | — |
| longshots_lt30 | 46 | 6 | `offset` | -0.0020 [-0.0141, +0.0046] | +0.0952* |
| longshots_lt30 | 46 | 6 | `offset_do_no_harm` | +0.0003 [+0.0000, +0.0007] | +0.0976* |
| near_boundary | 15 | 3 | `market_physics` | -0.0712 [-0.1171, -0.0241] | — |
| near_boundary | 15 | 3 | `offset` | +0.0037 [+0.0009, +0.0059] **QUALIFIES** | +0.0749* |
| near_boundary | 15 | 3 | `offset_do_no_harm` | +0.0005 [+0.0000, +0.0009] | +0.0717* |
| extreme_z | 2 | 1 | `market_physics` | +0.2199 unbounded (1 event) | — |
| extreme_z | 2 | 1 | `offset` | +0.0794 unbounded (1 event) | -0.1405 |
| extreme_z | 2 | 1 | `offset_do_no_harm` | +0.0000 unbounded (1 event) | -0.2199 |

## Exploratory comparisons

**30 comparisons** were computed (2 offset variants x 5 slices x 3 families). At 5% roughly
1.5 false positives are expected. Three intervals sit entirely above zero:

- `KXGOLDH favorites_gt70` — **6 events**
- `KXWTIH eval_band` — **4 events**
- `KXSILVERH extreme_z` (do-no-harm) — 26 events, effect **+0.0018** against a market already at Brier 0.0055

**None is authorized.** The first two rest on six and four independent events. The third has
adequate events but an effect indistinguishable from what 30 comparisons produce by chance.
**No post-hoc slice authorization**: a slice that looks good after the fact is not evidence.

## What the offset DID establish

It beats the unconstrained `market_physics` almost everywhere, often by a lot (silver
favorites 0.2493 -> 0.0473). That is the identity paying off: at `g=0` the offset IS the
market, so it can only lose ground it deliberately gives up, while the unconstrained form
must relearn the mid and demonstrably fails to (`market_logit` scores significantly WORSE
than the raw mid on both families).

Identity test: with `g`'s coefficients at zero the offset reproduces `market_raw` to
numerical tolerance. Pinned by a test.

Do-no-harm fallback: emits the market unchanged below 40 contracts. At gold's `extreme_z`
it posts `+0.0000` — declining to touch the region where the market is near-perfect.

## Architectural conclusion

Use the market as the **immutable baseline** and learn only a **regularized residual**
correction — defaulting that correction to zero until preregistered evidence proves
improvement.

## Research conclusion

Rearranging existing physics variables is unlikely to create meaningful edge. The next
useful effort is collecting genuinely **independent, time-valid signals** — not fitting
another mapping to substantially the same information.

