# Kalshi edge autopsy — where the edge actually is

**Issue:** [#169](https://github.com/Trucker2827/Open-Terminal/issues/169) ·
**Date:** 2026-07-27 · **Scope:** research only — no bot behaviour, threshold,
gate or ledger was changed, and every script runs read-only against the live
evidence directory.

All numbers below were produced at **as-of 2026-07-27T21:52Z** by the scripts in
`scripts/research/`. The evidence logs are live and still being written, so a
re-run will not reproduce these figures exactly; it will reproduce the method,
the tables and — the point of the exercise — the verdicts. Each section names
the command that produced its numbers.

---

## Verdict first

The strategy is not losing because it is badly tuned. It is losing because the
signal it is built on does not contain information the market price lacks.

1. **The calibrator does not beat the market mid.** Over 239 resolved contracts
   it scores Brier **0.1043** against the raw mid's **0.0989** — worse, not
   better, and worse in almost every slice we cut.
2. **Its own self-report says otherwise, and the self-report is measuring the
   wrong thing** — against a handicapped baseline, on a sample whose effective
   size is **~8–10 contracts** rather than the 500 it displays. This is a
   measurement defect in the calibrator's bookkeeping, not a modelling opinion.
3. **There is no recalibration headroom.** Walk-forward Platt and isotonic
   corrections make it *worse* (−0.0074 / −0.0096 Brier). Even a fully
   overfitted in-sample isotonic fit only reaches 0.0980, which merely ties the
   raw mid. The ceiling of correcting this signal is the market price.
4. **The quote lag is real and it is almost exactly the size of the Kalshi
   fee.** After a ≥3σ spot move, the Kalshi mid drifts a further **+2.07¢** in
   the move's direction over the next 15 seconds (t = 2.5). The half-spread is
   only **0.53¢** — but the fee is **1.55¢**, and 2.07 − 0.53 − 1.55 ≈ **0**.
   The book is slow; the fee eats the slowness.

The one structural thing this terminal has measured that the market does not
already price is the quote lag. It is currently not big enough to pay for
itself, which is a much more useful finding than "no edge found".

---

## Data inventory

`python3 scripts/research/inventory.py`

Evidence dir: `~/Library/Application Support/Open Terminal/Open Terminal/`.
These logs rotate **by size** (~67 MB) into a single `.1` sibling that the next
rotation overwrites. Retention is therefore a function of write rate, not of
elapsed time — which is the single most important constraint on this whole
report.

| Log | Rows | Retained span (UTC) | Hours | Cadence | Used for |
|---|---:|---|---:|---:|---|
| `kalshi-cf-benchmarks.jsonl` (+`.1`) | 162,161 | 07-23 01:18 → 07-27 21:52 | 116.6 | 2.6 s | BRTI index — Q1 events, Q2/Q3 outcomes, volatility |
| `kalshi-tickers.jsonl` (+`.1`) | 235,634 | 07-27 13:42 → 07-27 21:52 | **8.2** | 0.12 s | Kalshi top-of-book — Q1 quote series |
| `kalshi-bot-decisions.jsonl` | 87,811 | 07-24 15:48 → 07-27 21:52 | 78.1 | 3.2 s | `calibrated_p`, `market_mid`, bids/fills — Q2/Q3/Q4 |
| `kalshi-settlements.jsonl` | 170,920 | (no ms field) | — | — | recorded outcomes — Q2/Q3 ground truth |
| `kalshi-venue-features.jsonl` (+`.1`) | 88,243 | (no ms field) | — | — | **not read** — the obvious alternative for Q1, passed over: ~7.5 s cadence cannot resolve sub-minute repricing |

**The retention asymmetry is the finding.** The BRTI spot feed keeps 117 hours;
the Kalshi ticker feed, writing 20× faster, keeps 8. Any question needing both
is capped at ~8 hours. This is not a gap in what the terminal captures — it
captures everything needed, at sub-second resolution — it is a gap in what it
*keeps*. See follow-up F1.

### Outcome sourcing, and how it was validated

Joining the bot's watched tickers to the public settlement feed yields only
**81 of 463** contracts (17.5%), and coverage is erratic by day (0% on 07-26,
22–41% otherwise). ID formats match exactly — this is missing coverage in the
settlement feed, not a join bug.

Rather than accept n=81, outcomes are *derived* where the feed is silent: the
BRTI 60-second average at the contract's close versus the strike encoded in the
ticker. **This derivation was validated before use, not asserted:**

| Check | Result |
|---|---|
| Derived outcome vs Kalshi's recorded `result` | **42 / 42 agreement** |
| Reconstructed settlement value vs recorded `expiration_value` | mean abs error **$0.71**, max $11.93 |
| Instantaneous BRTI instead of the 60 s average | mean abs error $13.21 — rejected |

The 42/42 also confirms the ticker's close time is US-Eastern (EDT); that was
tested, not assumed. Derived outcomes are tagged `derived` in every row, and
every headline is repeated on the `recorded`-only subset as a robustness check.
Contracts resolvable by neither source (215, mostly `KXBTC15M` directional
contracts whose ticker carries no strike) are **dropped and counted**, never
imputed.

Final Q2/Q3 sample: **5,011 forecast rows across 239 contracts.** Outcomes were
resolved for 248 tickers (81 recorded, 167 derived); 9 of those contributed no
pre-close forecast row and drop out, leaving **77 recorded / 162 derived**
scored — the split the `by_outcome_source` table reports. A further 30,210 rows
were discarded for being logged at or after the close: a forecast made after the
outcome is known is not a forecast.

---

## Q1 — Kalshi quote lag versus spot

`python3 scripts/research/q1_quote_lag.py`

**Data:** BRTI (162k ticks) paired with the Kalshi ticker feed (126,129
two-sided quotes across 169 markets, 110 of them threshold). Paired window **8.2 hours**,
2026-07-27 13:42 → 21:52 UTC. 109,511 quote rows were dropped as one-sided — a
book with no ask has no midpoint, and inventing one from `1 - no_bid` would
fabricate the very quantity being measured.

**One whole market family is excluded, deliberately.** The 169 quoted markets
are **110 `KXBTCD` threshold** contracts (`-T63999.99`, settles YES above the
strike) and **59 `KXBTC` band** contracts (`-B65050`, a $100-wide band —
verified against the settlement feed, where exactly one band per event settles
YES, the one containing `expiration_value`). Only the threshold family is
analysed, and the filter counts it (`excluded_band_market`: 2,065 pairs at 2σ,
708 at 3σ) rather than silently dropping it. The reason is not sample size: the
statistic below multiplies a mid change by the **sign of the spot move**, which
presumes YES is monotone in spot. For a band it is not — a large up-move can
push spot straight through and *out* of the band, lowering its YES while the
move was upward. Including band markets would not be a bigger measurement, it
would be a wrong one. (The calibrator declines them for the same reason:
`extract_features` returns None for range markets.)

**Method — deliberately model-free.** Pricing a Gaussian implied probability off
fresh spot and calling the distance to the Kalshi mid an "edge" mostly measures
the Gaussian. So the primary statistic needs no model: *after a spot move that
has already completed by time t, does the Kalshi mid keep moving in that
direction?* If yes, the book was stale at t and the staleness is denominated in
cents, directly comparable to what you would pay to take it.

Events: |60-second BRTI log return| ≥ k·σ, σ = trailing 30-minute realized
per-minute volatility computed from BRTI itself (no evidence file carries a
ready-made vol number for this window). Events ≥5 minutes apart. Markets:
`KXBTCD` threshold contracts closing in 2–60 minutes, mid in [0.05, 0.95].

### Aligned mid drift after the move (contested markets)

Positive = the book moved the way the *already-known* spot move implied.

| k | Events | Markets | Horizon | n | Mean drift | t | Half-spread | Fee | Net if taking | Net vs fee only |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2σ | 22 | 56 | 5 s | 116 | +0.53¢ | 2.42 | 0.53¢ | 1.52¢ | −1.52¢ | −0.99¢ |
| 2σ | 22 | 56 | **15 s** | 116 | **+1.22¢** | **3.52** | 0.53¢ | 1.52¢ | −0.83¢ | −0.30¢ |
| 2σ | 22 | 56 | 30 s | 116 | +0.55¢ | 0.90 | 0.53¢ | 1.52¢ | −1.50¢ | −0.97¢ |
| 2σ | 22 | 56 | 60 s | 116 | +0.47¢ | 0.89 | 0.53¢ | 1.52¢ | −1.58¢ | −1.05¢ |
| 3σ | 8 | 28 | 5 s | 44 | +1.08¢ | 2.12 | 0.53¢ | 1.55¢ | −1.00¢ | −0.47¢ |
| 3σ | 8 | 28 | **15 s** | 44 | **+2.07¢** | **2.50** | 0.53¢ | 1.55¢ | −0.01¢ | **+0.52¢** |
| 3σ | 8 | 28 | 30 s | 44 | +1.84¢ | 2.55 | 0.53¢ | 1.55¢ | −0.24¢ | +0.30¢ |
| 3σ | 8 | 28 | 60 s | 44 | +0.44¢ | 0.29 | 0.53¢ | 1.55¢ | −1.64¢ | −1.11¢ |

Horizons ≥120 s are excluded from this table: over minutes, spot keeps moving,
so the statistic stops measuring lag and starts measuring momentum. (They are in
the JSON, flagged as such.)

**Verdict.** The lag is real and statistically clear at the 15-second horizon
(t = 3.52 at 2σ, t = 2.50 at 3σ), and it decays by 60 seconds — the book catches
up inside a minute. The magnitude is the problem: after a 3σ move the mid drifts
2.07¢, and taking that quote costs 2.08¢. **The Kalshi fee, not the spread, is
the wall** — the market is tight (half-spread 0.53¢) and the fee is 1.55¢, three
times larger. A strategy that crosses the spread to harvest this earns nothing;
one that could capture it at fee-only cost would clear roughly +0.5¢ per event
at 3σ. That is a thin, real, and *structurally different* edge from the one the
bot is currently pursuing.

**The honest caveat: 8 events at 3σ, 22 at 2σ.** This is directionally
suggestive, not established. The t-statistics treat (event, market) pairs as
independent, and markets sharing an event are not — the effective n is closer to
the event count than the sample count. This is precisely the measurement that a
longer paired window would settle, and it is why F1 is the first follow-up.

---

## Q2 — Calibrator error anatomy

`python3 scripts/research/q2_calibrator_error_anatomy.py`

**Data:** 5,011 forecast rows / 239 contracts (see inventory above).

### The bookkeeping defect, first

`spot-calibrator-state.json` cannot answer this question on its own: it keeps
only the trailing `BRIER_WINDOW = 500` `[probability, outcome]` pairs and
attaches no ticker, no timestamp and no features to any of them. Worse for
interpretation, `settle_cycle` appends **one pair per observation**, and each
contract contributes up to `MAX_OBS_PER_TICKER = 60`:

| What the calibrator reports | Value | What it actually means |
|---|---:|---|
| `training_samples` | 500 | **~8–10 *contracts*** of heavily correlated rows |
| `resolved_contracts` | 976 | lifetime count, unrelated to the 500 scored |
| `brier_full` | 0.0465 | over that ~8–10 contract effective sample |
| `brier_market_baseline` | 0.0416 | **a trained 1-feature logit on the mid, not the mid** |

The ~8–10 is **measured, not assumed from `MAX_OBS_PER_TICKER`**. The state
file's own `pending` map holds the observation list each ticker will train on
when it settles; across the 35 pending contracts at as-of time that distribution
is min 2 / median 60 / mean 49.4 / max 60 observations per contract. So 500
stored pairs is **10.1 contracts at the mean, 8.3 at the dense end**. (The
theoretical bound is 8–500, printed as `implied_contract_count_bound`; the
observed cadence sits hard against the dense end of it.)

`adds_value_over_market` gates on `len(brier_full) >= 100` — a threshold those
500 correlated rows clear after about two contracts settle. **This fully
explains the flip-flopping around zero** described in the issue: the flag is a
noisy comparison on a handful of contracts, re-evaluated constantly. It is a
measurement defect, and it is cheap to fix (F2).

Note also that at as-of time the calibrator was losing even to its own
handicapped baseline (0.0465 vs 0.0416).

### Overall, on the reconstructed history

| Sample | Contracts | Rows | Brier (calibrated) | Brier (raw mid) | Δ |
|---|---:|---:|---:|---:|---:|
| All | 239 | 5,011 | 0.1043 | **0.0989** | **+0.0054** |
| Recorded settlements only | 77 | 1,036 | 0.0974 | **0.0956** | +0.0018 |

The recorded-only subset agrees in sign and direction, so the derived outcomes
are not driving the conclusion.

### By time to expiry

| Bucket | Contracts | Rows | Calibrated | Mid | Δ |
|---|---:|---:|---:|---:|---:|
| 0–2 m | 68 | 100 | 0.1340 | 0.1120 | +0.0221 |
| 2–5 m | 91 | 179 | 0.1421 | 0.1159 | +0.0262 |
| 5–15 m | 142 | 828 | 0.1311 | 0.1174 | +0.0137 |
| 15–30 m | 142 | 1,185 | 0.0837 | 0.0754 | +0.0082 |
| 30–60 m | 181 | 2,701 | 0.1022 | 0.1026 | −0.0004 |

The calibrator is worst exactly where the bot most wants to act — near expiry.
The one non-losing bucket (30–60 m) is a tie, not a win.

### By volatility regime (terciles of trailing per-minute vol: 2.45 / 3.82 bps)

| Regime | Contracts | Rows | Calibrated | Mid | Δ |
|---|---:|---:|---:|---:|---:|
| low | 89 | 1,636 | 0.0963 | 0.0891 | +0.0072 |
| mid | 112 | 1,634 | 0.1008 | 0.0858 | +0.0151 |
| **high** | **116** | **1,632** | **0.1151** | **0.1218** | **−0.0067** |

**This is the only non-trivial slice where the calibrator beats the market**, on
116 contracts. When the market is moving fast the mid degrades (0.0891 → 0.1218)
faster than the calibrator does. That is consistent with Q1: in high vol the
Kalshi quote is stale, and a physics-based estimate is briefly better than a
lagging price. It is the second-most promising thing in this report (F3).

### By distance from strike (in sigmas)

| Bucket | Contracts | Rows | Calibrated | Mid | Δ |
|---|---:|---:|---:|---:|---:|
| < −2σ | 91 | 901 | 0.0021 | 0.0024 | −0.0002 |
| −2..−1σ | 83 | 589 | 0.0657 | 0.0625 | +0.0033 |
| −1..0σ | 72 | 597 | 0.2127 | 0.1996 | +0.0131 |
| 0..1σ | 64 | 621 | 0.2627 | 0.2467 | +0.0160 |
| 1..2σ | 72 | 568 | 0.1116 | 0.1064 | +0.0051 |
| > 2σ | 76 | 771 | 0.0058 | 0.0061 | −0.0003 |

The two "wins" are the deep tails where both Briers are ~0.002 — everyone is
right about a contract that is 2σ from its strike. All the contested mass
(|d| < 1σ) is a loss.

### Tail confidence — is it Gaussian-overconfident?

| Band | Contracts | Rows | Mean predicted | Observed | Calibrator gap | Mid's gap, same band |
|---|---:|---:|---:|---:|---:|---:|
| p < 0.02 | 43 | 338 | 0.0137 | 0.0000 | +0.0137 | +0.0148 |
| 0.02–0.05 | 88 | 1,006 | 0.0331 | 0.0169 | +0.0162 | +0.0315 |
| 0.05–0.10 | 77 | 548 | 0.0714 | 0.0967 | −0.0253 | +0.0579 |
| 0.10–0.90 | 128 | 1,591 | 0.5103 | 0.4664 | +0.0439 | +0.0695 |
| **0.90–0.95** | **71** | **484** | **0.9286** | **0.8120** | **+0.1167** | +0.0205 |
| 0.95–0.98 | 84 | 799 | 0.9685 | 0.9687 | −0.0002 | −0.0076 |
| p > 0.98 | 40 | 245 | 0.9827 | 0.9918 | −0.0091 | −0.0149 |

**Not the classic Gaussian tail failure** — the extreme tails (p<0.02, p>0.98)
are fine, and are no worse than the market's. The damage is concentrated in one
band: **0.90–0.95, where it predicts 92.9% and observes 81.2%**, an 11.7-point
overconfidence on 71 contracts, while the market mid in that same band is off by
only 2.1 points. This is a calibrator-specific defect, not a shared market
failure, and it sits exactly where a "buy the confident favourite" rule fires —
which is the shape of the bot's actual losses in Q4.

**Verdict.** The calibrator's error is not concentrated in a regime that can be
carved out and traded. It loses to the raw mid nearly everywhere, worst near
expiry and in the contested zone around the strike, with one specific
overconfidence band (0.90–0.95) and one genuinely interesting exception
(high volatility).

---

## Q3 — Recalibration headroom

`python3 scripts/research/q3_recalibration_headroom.py`

**Method.** Platt scaling and pool-adjacent-violators isotonic regression, the
arena overlay pattern from PR #110. **Splits are by contract close time, never
by row** — a contract contributes up to 56 rows sharing one outcome, so a
row-wise split would leak the same outcome into both halves and manufacture
out-of-sample "improvement" from nothing. Every test set is strictly in the
future of its training set.

| Evaluation | Test contracts | Raw calibrated | Platt | Isotonic | Raw mid | Beats mid? |
|---|---:|---:|---:|---:|---:|---|
| Half / half | 120 | 0.1113 | 0.1090 | 0.1082 | 0.1099 | marginally (−0.0017) |
| **Walk-forward (5 folds)** | **219** | **0.1043** | **0.1118** | **0.1140** | **0.0990** | **no (+0.0128)** |
| In-sample (overfit yardstick) | 239 | 0.1043 | 0.1017 | 0.0980 | 0.0989 | ties |

**Verdict: there is no honest recalibration headroom.** The half/half split
shows a small apparent gain; the stricter walk-forward — five expanding windows
— reverses it entirely, with both correctors performing *worse* than the raw
calibrator (Platt −0.0074, isotonic −0.0096) and neither approaching the mid.
The correction is fitting regime noise that does not persist into the next fold.

The in-sample row is the decisive one. Fitting an isotonic correction on the
very rows it is scored against — the most flattering evaluation possible, which
can drive Brier arbitrarily low given enough resolution — reaches only 0.0980
against the mid's 0.0989. **Even with perfect hindsight, recalibrating this
signal merely ties the market price.** The calibrator is not miscalibrated; it
is uninformative. No amount of post-processing creates information that is not
in the input, and this is the measurement that says so.

---

## Q4 — The bot's own bids

`python3 scripts/research/q4_bid_postmortem.py`

**Data:** 87,801 decision rows, 2026-07-24 15:48 → 2026-07-27 21:40 UTC.

### Funnel

| Stage | Count |
|---|---:|
| Decision rows | 87,785 |
| Passes | 87,004 |
| Bids placed | 395 |
| Cancels | 343 |
| Fills | 43 (10.9% of bids) |
| **Settled positions** | **15** |

Top pass reasons: `EDGE_BELOW_THRESHOLD` 58,625 · `NO_RUNWAY` 17,838 ·
`SESSION_BUDGET_BLOCKS_BID` 6,193 · `ALREADY_HELD` 2,335.

### Every settled position

| Settled (UTC) | Ticker | Side | Ct | Price | Stake | Won | P&L | Rested (s) |
|---|---|---|---:|---:|---:|---|---:|---:|
| 07-24 16:03 | KXBTCD-26JUL2412-T63999.99 | YES | 2 | 0.81 | 1.62 | ✓ | +0.35 | — |
| 07-25 04:34 | KXBTC15M-26JUL241215-15 | NO | 2 | 0.76 | 1.52 | ✗ | −1.55 | — |
| 07-25 04:34 | KXBTCD-26JUL2413-T63999.99 | NO | 4 | 0.50 | 2.00 | ✓ | +1.93 | — |
| 07-25 04:34 | KXBTCD-26JUL2413-T64099.99 | NO | 2 | 0.67 | 1.34 | ✓ | +0.62 | — |
| 07-27 06:19 | KXBTCD-26JUL2702-T65199.99 | YES | 2 | 0.82 | 1.64 | ✓ | +0.33 | 66.1 |
| 07-27 06:19 | KXBTC15M-26JUL270130-30 | YES | 2 | 0.83 | 1.66 | ✓ | +0.32 | 190.9 |
| 07-27 08:00 | KXBTC15M-26JUL270400-00 | YES | 3 | 0.63 | 1.89 | ✗ | −1.94 | 216.3 |
| 07-27 08:16 | KXBTC15M-26JUL270415-15 | YES | 3 | 0.52 | 1.56 | ✗ | −1.62 | 139.2 |
| 07-27 08:45 | KXBTC15M-26JUL270445-45 | YES | 4 | 0.45 | 1.80 | ✗ | −1.87 | 153.5 |
| 07-27 10:15 | KXBTC15M-26JUL270615-15 | YES | 3 | 0.58 | 1.74 | ✗ | −1.80 | 215.9 |
| 07-27 10:30 | KXBTC15M-26JUL270630-30 | YES | 2 | 0.67 | 1.34 | ✓ | +0.62 | 137.2 |
| 07-27 17:15 | KXBTC15M-26JUL271315-15 | NO | 3 | 0.59 | 1.77 | ✓ | +1.17 | 131.5 |
| 07-27 17:33 | KXBTC15M-26JUL270245-45 | YES | 2 | 0.85 | 1.70 | ✓ | +0.28 | 195.2 |
| 07-27 19:30 | KXBTC15M-26JUL271530-30 | YES | 4 | 0.50 | 2.00 | ✗ | −2.07 | 65.5 |
| 07-27 20:03 | KXBTCD-26JUL2716-T64799.99 | NO | 3 | 0.58 | 1.74 | ✗ | −1.80 | 67.5 |

**Totals:** 8 wins / 7 losses (53.3% win rate), stake $25.32, fees $0.71,
**net −$7.03**, mean −$0.47 per position.

### Payoff shape — descriptive, not inferential

| | Mean entry price | Mean P&L |
|---|---:|---:|
| Winners (8) | 0.7175 | +$0.70 |
| Losers (7) | 0.5743 | −$1.81 |

A 53% win rate coexisting with a $7 loss is the arithmetic of buying favourites:
a contract bought at 0.83 wins four times in five and pays 0.17, while a loss
pays the whole stake. This is mechanical, not statistical — and it lines up with
the Q2 finding that the calibrator is 11.7 points overconfident precisely in the
0.90–0.95 band. Reading it as "the bot should buy underdogs" would be an n=15
overfit; reading it as "the bot's confident-favourite forecasts are the ones
that are wrong" is supported by Q2's 71-contract band.

### What cannot be answered

**Winners vs losers is not measurable at n = 15.** Any split of this sample
produces groups of single digits, where a 20–30 point difference in any rate is
the expected size of chance alone. No discriminating feature is reported because
none can be measured. The sealed gate requires **300** settled bids; the ledger
has 15.

**The $0.03 adverse-selection premium (`rest_premium_usd`,
`KalshiBotDecision.h:191`, issue #165) is neither confirmed nor refuted.**
Sizing it requires comparing the realized edge of resting fills against crossing
fills, and **all 43 fills came from resting quotes — there is not one settled
crossing fill in the ledger.** There is no contrast to measure. (Of 395 bids,
77 were tagged `rest`, 14 `cross`, 304 untagged; the ledger rows do not journal
`rest_premium_usd` itself in this window.)

---

## Follow-ups, ranked by measured promise

Filed as `agent-ready`, `finn`. Each cites the numbers above as its Why.

**F1 — Retain a downsampled paired spot + top-of-book series.** *(highest)* — filed as [#170](https://github.com/Trucker2827/Open-Terminal/issues/170)
Q1 found the only structural edge in this report and could only measure it over
**8.2 hours / 8 events at 3σ**, because `kalshi-tickers.jsonl` rotates at 67 MB
and keeps 8 hours while BRTI keeps 116. Capture already exists at sub-second
resolution — nothing new needs recording, only *keeping*. A compact retained
series (1 Hz BRTI + per-market top-of-book on change, threshold contracts inside
60 minutes of expiry) would take the 3σ estimate from 8 events to hundreds
within a week and settle whether the +0.52¢ fee-only margin is real.

**F2 — Fix the calibrator's Brier bookkeeping** ([#171](https://github.com/Trucker2827/Open-Terminal/issues/171))**.** Cheap, and it removes a live
source of false confidence. Three defects, all in `spot_calibrator.py`:
`brier_full` stores one pair per *observation* (measured median 60/contract) so
`training_samples: 500` is really ~8–10 contracts;
`adds_value_over_market` gates on ≥100 of those correlated rows, which two
settled contracts satisfy; and `brier_market_baseline` is a *trained* 1-feature
logit rather than the raw mid, so "beats the market" does not mean beating the
market. Score per contract, and compare against the raw mid.

**F3 — Measure the high-volatility exception properly** ([#172](https://github.com/Trucker2827/Open-Terminal/issues/172))**.** The single
non-trivial slice where the calibrator beats the mid: Brier **0.1151 vs
0.1218** over 116 contracts in the top volatility tercile (>3.82 bps/min),
while it loses in both lower terciles. It is also mechanistically consistent
with Q1 — in fast markets the Kalshi quote is stale and a physics estimate is
briefly better than a lagging price. Worth isolating with a proper
walk-forward on the vol-conditioned subset before anything is built on it; the
present result is one cut of one 3-day window.

**Not recommended:** any further recalibration work on the current signal (Q3
shows the ceiling is the mid, even with hindsight), and any conclusion drawn
from the 15 settled bids (Q4).

---

## Reproducing this

```
python3 scripts/research/inventory.py
python3 scripts/research/q1_quote_lag.py
python3 scripts/research/q2_calibrator_error_anatomy.py
python3 scripts/research/q3_recalibration_headroom.py
python3 scripts/research/q4_bid_postmortem.py
```

Each prints JSON including its own `as_of_utc`, the files and row counts it
read, and every number quoted above. All five open evidence strictly read-only;
none imports or calls `spot_calibrator.run_once()`, which would rewrite the
operator's live calibrator state as a side effect of reading it. Point them at a
copy with `OPENTERMINAL_EVIDENCE_DIR` if you want to freeze the inputs.
