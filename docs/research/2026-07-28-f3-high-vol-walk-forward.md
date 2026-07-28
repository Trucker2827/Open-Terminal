# F3 — the high-volatility exception, measured properly

**Issue:** [#172](https://github.com/Trucker2827/Open-Terminal/issues/172) ·
**Follows:** [#169](https://github.com/Trucker2827/Open-Terminal/issues/169),
`docs/research/2026-07-27-kalshi-edge-autopsy.md` · **Date:** 2026-07-28 ·
**Scope:** research only — no bot behaviour, threshold, gate or ledger was
changed, and the script runs read-only against the live evidence directory.

Every number below was produced at **as-of 2026-07-28T00:18Z** by
`python3 scripts/research/f3_high_vol_walk_forward.py`. The evidence logs are
live and rotate by size, so a re-run will not reproduce these figures exactly;
it will reproduce the method and the verdict.

Throughout, **Δ = Brier(calibrated) − Brier(raw mid)**, so a **negative Δ means
the calibrator beat the market**.

---

## Verdict first

**The exception does not survive.** It is not refuted at any single cut point —
it is dissolved by asking what the sample actually is.

1. **It reverses when each contract gets one vote.** Pooled over rows the
   calibrator wins by **−0.0066** (0.1143 vs 0.1209), reproducing #169's cell.
   Weighted one-contract-one-vote it **loses by +0.0021** (0.0883 vs 0.0862).
   The published win is a row-weighting artifact: the contracts contributing 42
   rows are not the same contracts as the ones contributing 1.
2. **It is not statistically distinguishable from zero.** The paired
   per-contract difference over all 116 contracts is **t = 0.35**, and 66 of 116
   contracts favour the calibrator (exact two-sided sign test **p = 0.16**).
3. **The 116 contracts are 3 volatility episodes, and one of them is 96% of the
   rows.** This is the finding. High volatility arrives in bursts, so a
   walk-forward split by close time cannot spread this subset across time — the
   time simply is not there. **The effective n is 1 episode, not 116
   contracts.**
4. **The two smaller episodes both go the other way** (+0.0175 and +0.0240).
   Every bit of the exception lives in one 4.6-hour window on 2026-07-27.

The honest reading: **there is no measured high-volatility exception — there is
one high-volatility afternoon.** That is a statement about evidence retention,
not about the calibrator, and it is the same constraint F1 ([#170](https://github.com/Trucker2827/Open-Terminal/issues/170)) exists to
relieve.

The control result confirms the pipeline is not simply blind: on the low/mid
subset the calibrator loses by **+0.0112 with t = 3.13**, the direction and
strength #169 reported. When there is a real effect in this data, this machinery
reports it.

**A note on the script's own headline, which is deliberately weaker.** Running
the script prints `The exception is WEIGHTING-DEPENDENT …`, because that string
is assembled mechanically from the two Brier signs and is not allowed to editorialise.
The report states the stronger conclusion because of what the same
`verdict` string goes on to say — *"but those contracts are only 3 volatility
episode(s), the largest carrying 96% of the rows — the effective n"*. A
weighting-dependent result measured over one episode is not a surviving
exception. The difference between the two sentences is judgement about sample
structure, applied on top of the arithmetic, and it is stated here rather than
quietly folded into the script's output.

---

## Method

**The threshold was frozen before the walk-forward was run, and this is what
that means.** `FROZEN_VOL_THRESHOLD_BPS = 3.82` is a literal in the script,
carried over verbatim from the tercile boundary #169 published (report table
"By volatility regime", as-of 2026-07-27T21:41Z). Nothing in this script
recomputes it from the data it scores. Its honest caveat: **#169 derived it from
a window that overlaps this one, so the threshold is frozen but not
independent.** The sensitivity sweep below is what covers that exposure.

**The split is by contract close time, never by row.** A contract contributes up
to 42 rows in this subset, all sharing one outcome; #169's Q3 showed a row-wise
split manufactures out-of-sample gain from leakage. Fold boundaries are
additionally *snapped to close-time groups*, so contracts closing at the same
instant never land in different folds and each fold is strictly later than the
last. Both properties are asserted in
`openmarketterminal-qt/tests/test_f3_high_vol_walk_forward.py`, and the script
re-checks them at runtime (`leakage_check`: 0 tickers in more than one fold,
folds strictly time-ordered).

**Nothing is fitted, and the report claims nothing that would require it.**
Comparing a recorded `calibrated_p` against a recorded `market_mid` estimates no
parameters. The fold structure therefore does **not** protect against model
overfitting — there is no model to overfit. It buys exactly two things: the
threshold was fixed a priori, and per-fold sign stability is visible. It follows
that **the pooled figure is by construction identical to scoring the whole
subset at once**; it is printed per fold *and* pooled because the issue asks for
both, not because pooling is a separate held-out estimate.

**Sample.** 5,011 forecast rows / 239 contracts (the #169 reconstruction,
re-derived at this run's as-of). Of these, **1,651 rows across 116 contracts**
exceed 3.82 bps/min. 109 rows carried no computable volatility and were dropped
and counted, never assigned to a side. Derived outcomes remain validated at
**45/45 agreement** against recorded settlements.

---

## The sample is three episodes

`high_vol_episode_structure` — rows separated by more than an hour with no
selected row start a new episode.

| Episode | Window (UTC) | Hours | Rows | Contracts | Calibrated | Raw mid | Δ |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | 07-24 15:48 → 16:03 | 0.3 | 23 | 23 | 0.0839 | 0.0664 | **+0.0175** |
| **2** | **07-27 12:27 → 17:01** | **4.6** | **1,581** | **82** | **0.1147** | **0.1225** | **−0.0079** |
| 3 | 07-27 19:05 → 19:11 | 0.1 | 47 | 11 | 0.1167 | 0.0927 | **+0.0240** |

`volatility_by_utc_day` — over **all** scored rows, so the two empty days are
shown to be a calm market rather than a join that lost them. This is a
row-level diagnostic of where the fast markets were, not a scoring cell: it
reports no Brier and therefore carries no contract count, unlike every table
below it.

| Day (UTC) | Rows with vol | Median | Max | Rows > 3.82 |
|---|---:|---:|---:|---:|
| 2026-07-24 | 23 | 6.39 | 7.60 | 23 (100%) |
| 2026-07-25 | 298 | 1.23 | **1.62** | **0** |
| 2026-07-26 | 684 | 1.35 | **2.36** | **0** |
| 2026-07-27 | 3,897 | 3.60 | 11.31 | 1,628 (41.8%) |

07-25 and 07-26 could not have contributed a single row: their *maximum*
per-minute volatility, 1.62 and 2.36 bps, sits below the cut. Two of the four
retained days are structurally absent from this measurement.

Contracts inside one episode are priced off a single BTC path. Treating 82 of
them as 82 independent observations of "the calibrator in fast markets" is the
same error as treating 42 rows of one contract as 42 independent forecasts —
which is the error #169 was written to correct. **Episode 2 is the entire
finding, and it is one afternoon.**

---

## Walk-forward, high-volatility subset (threshold frozen at 3.82 bps/min)

Five folds, contracts ordered by close time. Per-fold contract counts are 12–31,
so a per-fold sign flip is noise and is not read as a finding.

| Fold | Close span (UTC) | Contracts | Rows | Calibrated | Raw mid | Δ (row) | Δ (contract-mean) | Contracts won |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 07-24 16:00 → 17:00 | 23 | 23 | 0.0839 | 0.0664 | +0.0175 | +0.0175 | 13/23 |
| 2 | 07-27 12:30 → 14:00 | 28 | 322 | 0.1475 | 0.1490 | −0.0015 | −0.0042 | 16/28 |
| 3 | 07-27 14:30 → 15:00 | 22 | 546 | 0.1404 | 0.1587 | −0.0183 | −0.0168 | 17/22 |
| 4 | 07-27 15:30 → 17:00 | 31 | 712 | 0.0802 | 0.0828 | −0.0026 | +0.0057 | 14/31 |
| 5 | 07-27 17:15 → 20:00 | 12 | 48 | 0.1162 | 0.0943 | +0.0219 | +0.0123 | 6/12 |
| **Pooled** | **07-24 → 07-27** | **116** | **1,651** | **0.1143** | **0.1209** | **−0.0066** | **+0.0021** | **66/116** |

**3 of 5 folds favour the calibrator on rows; 2 of 5 on contract means.**
Observations per contract in this subset: min 1, median 10, mean 14.2, max 42 —
the spread that makes the two weightings disagree.

Paired per-contract difference, pooled: mean Δ **+0.0021**, standard error
0.0060, **t = 0.35**, sign test **66/116, p = 0.16**. Neither is close to
conventional significance, and both overstate their own confidence because
contracts within an episode are correlated.

---

## Sensitivity — does it exist only at 3.82?

| Threshold (bps/min) | Contracts | Rows | Δ (row) | Δ (contract-mean) | Paired t | Folds won |
|---:|---:|---:|---:|---:|---:|---:|
| 2.45 | 193 | 3,274 | +0.0042 | +0.0098 | +2.15 | 1/5 |
| 3.00 | 164 | 2,535 | −0.0016 | +0.0034 | +0.77 | 3/5 |
| 3.40 | 146 | 2,300 | −0.0018 | +0.0044 | +0.87 | 2/5 |
| **3.82** (frozen) | **116** | **1,651** | **−0.0066** | **+0.0021** | **+0.35** | **3/5** |
| 4.25 | 102 | 1,494 | −0.0088 | +0.0007 | +0.11 | 2/4 |
| 5.00 | 90 | 1,388 | −0.0137 | −0.0017 | −0.24 | 3/5 |
| 6.00 | 75 | 1,225 | −0.0186 | −0.0054 | −0.66 | 3/4 |
| 8.00 | 48 | 758 | −0.0208 | −0.0235 | −2.55 | 3/3 |

**This is the one place the finding does better than expected.** The row-weighted
Δ is not a spike at one cut point — it is monotone in the threshold, growing
from +0.0042 to −0.0208 as the cut tightens. A pure cut-point artifact would not
behave that way, and this is genuine (if weak) support for the mechanism.

Two things stop it being more than that. The contract-mean Δ stays within
±0.005 of zero until 5.00 and only turns clearly negative at **8.00 bps/min**,
where n = 48 contracts. And every one of those tighter cuts is a *subset of
episode 2* — tightening the threshold does not add independent evidence, it
discards the two episodes that disagreed. The t = −2.55 at 8.00 is computed over
48 contracts from a single afternoon and should not be read as a p-value.

---

## Recorded settlements only

Derived outcomes are not driving the result: the recorded-only subset agrees in
sign on **both** weightings.

| Cell | Contracts | Rows | Calibrated | Raw mid | Δ (row) | Δ (contract-mean) | Paired t |
|---|---:|---:|---:|---:|---:|---:|---:|
| High-vol, recorded only | 42 | 521 | 0.0779 | 0.0898 | −0.0120 | −0.0061 | −0.65 |

Sign test 23/42 contracts, **p = 0.64**. Per-fold (5 folds, 5–12 contracts each):
−0.0067, −0.0444, −0.0211, +0.0292, +0.0170 — **3 of 5** favour the calibrator.
Observations per contract: min 1, median 5, mean 12.4, max 42.

This subset is the strongest form of the exception on offer, and it is still
n = 42 contracts drawn from the same episodes, at t = −0.65.

---

## Control — the low/mid regime (labelled control, not a finding)

The same machinery, same folds, on the complement where #169 found the
calibrator losing. Its purpose is to distinguish "the exception died" from "the
fold code has a sign bug".

| Cell | Contracts | Rows | Calibrated | Raw mid | Δ (row) | Δ (contract-mean) | Paired t | Folds won |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Low/mid (≤ 3.82) | 158 | 3,251 | 0.0988 | 0.0876 | **+0.0112** | **+0.0141** | **+3.13** | 2/5 |

The control moves the expected way and does so strongly. Note the contrast that
matters: **t = 3.13 on the control versus t = 0.35 on the high-vol subset.** The
low/mid loss is spread over 158 contracts across all four days; the high-vol
"win" is one afternoon.

---

## The stricter freeze that was tried and rejected

The more rigorous-sounding alternative is to re-derive the upper tercile from an
in-time training prefix and evaluate only on the remainder. It was run, and it
is reported rather than quietly dropped, because the reason it fails is itself
informative.

| | Value |
|---|---|
| Prefix | 79 contracts, close 07-24 16:00 → 07-27 08:00 |
| Tercile boundary derived from prefix | **1.81 bps/min** |
| Share of later rows it admits | **95.9%** (3,451 / 3,597) |
| Share a true tercile would admit | 33.3% |

**Per-minute BTC volatility is strongly non-stationary across this window**, so a
boundary learned on the calm 07-25/07-26 stretch is not a tercile of the
evaluation period — it admits nearly everything. Scoring it would mean measuring
almost the whole sample while calling it the high-volatility exception. The
frozen literal, with its overlap caveat stated, is the more honest of two
imperfect options.

This is also the cleanest statement of the underlying problem: **a "volatility
tercile" computed on a 3-day window is a fact about the window, not about a
regime.**

---

## What this does and does not license

- **It does not license a vol-gated trading rule.** The issue named that as a
  non-goal contingent on this surviving; it did not survive.
- **It does not refute the mechanism.** Q1's quote-lag finding stands on its own
  evidence, and the monotone sensitivity trend is weakly consistent with it. What
  is refuted is the claim that the *calibrator* has been measured to beat the mid
  in fast markets. It has been measured over one afternoon.
- **It sharpens F1's case.** The blocker is not analysis, it is retention: 116
  contracts that are 3 episodes cannot be fixed by a better estimator. #170's
  retained paired series is what would turn this into a real measurement, and
  the same argument applies to the calibrator subset, not only to Q1's quotes.
- **Nothing was changed.** No bot behaviour, threshold, gate, ledger or
  calibrator state. The script opens evidence read-only and never calls
  `spot_calibrator.run_once()`.

**Re-run when there are ≥10 volatility episodes**, not when there are more
contracts. Contract count is the number this analysis has shown to be
misleading; episode count is the one to gate on.

---

## Reproducing this

```
python3 scripts/research/f3_high_vol_walk_forward.py
```

Prints JSON including its own `as_of_utc`, the files and row counts it read, the
frozen threshold and its provenance, every table above, and the runtime
`leakage_check`. Point it at a copy with `OPENTERMINAL_EVIDENCE_DIR` to freeze
the inputs.

The methodology guarantees are regression-tested in
`openmarketterminal-qt/tests/test_f3_high_vol_walk_forward.py` (36 hermetic
tests, `ctest -R test_f3_high_vol_walk_forward`): contract-wise folds, strict
time ordering, close-time groups never straddling a boundary, threshold
selection depending only on a row's own volatility, missing volatility read as
missing, empty cells scoring `None` rather than a flattering `0.0`, and the
episode clustering that produces the effective n.
