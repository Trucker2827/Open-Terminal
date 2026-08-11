# Scope: split calibrator evidence and trust by exact series family

**Status:** scope only — no code written. Blocker for commodity-family preregistration.

## Correction to the stated safety status

> "commodities trust is currently false"

That is true for two of the three commodity reports and **false for the one that matters**:

| report | families pooled | `adds_value_on_bet_eligible` | scored |
|---|---|---|---|
| `commodities-15m-calibrator.json` | KXGOLD15M, KXSILVER15M, KXWTI15M | false | 226 |
| `commodities-daily-calibrator.json` | KXGOLDD, KXSILVERD, KXWTI | false | 29 |
| **`commodities-hourly-calibrator.json`** | **KXGOLDH, KXSILVERH, KXWTIH** | **TRUE** | **500** |

`commodities-hourly` is trusted **right now**, pooled across gold, silver and oil (Brier 0.1114 vs market 0.1808 — measured on the pooled set). It is the flag that authorized the 2026-08-10 burst: **KXGOLDH 58 bids, KXSILVERH 41, KXWTIH 1**.

So this is not a latent defect awaiting preregistration. It is **actively authorizing bids in three different underlyings from one pooled trust decision today**. No money is at risk (paper only), but the damage is to the evidence base: the paper record the sealed gate will later judge is being generated under pooled authorization.

**Immediate containment option (decide before the full fix):** stop the commodity families bidding until the split lands. Cheapest honest lever is to stop the `commodities-hourly` calibrator agent so the report goes stale — `KalshiBotDecision` already refuses a stale report (`REPORT_STALE`). That is a one-line launchctl action, reversible, and needs no code.

## Blast radius

**Multi-family reports (must split): 3** — the commodities 15m / hourly / daily reports above.

**Single-family reports that do not declare a family: 3** — `calibrator.json`, `kxbtc15m-calibrator.json` (both `families: null`) and `kxbtc-daily-calibrator.json` (`families: ["KXBTC"]`). These are not pooled, but once bidding *requests trust by family and refuses on absence*, a report that declares no family would refuse everything. They need an explicit `families` entry as part of the same change, or the rule cannot be turned on.

**Naming inconsistency to resolve before sealing anything:** daily WTI is `KXWTI`, while 15m/hourly are `KXWTI15M` / `KXWTIH`. `family_of()` handles all three correctly (prefix before the first `-`), but an operator preregistering "the WTI families" will guess wrong. Enumerate them from the live universe, do not hand-write them.

**Model state is one blob per report, not per family.** `commodities-hourly-calibrator-state.json` is 9.0 MB, `commodities-daily` 3.0 MB, holding keys like `contract_scores_full`, `contract_scores_eligible_full`, `market_trained_logit`. Splitting trust without splitting *this* would leave three families sharing one fitted model — the "updating Gold cannot change Silver" test would fail. This is the largest single piece of the work.

## Implementation order (as specified, with the seams)

1. **Split producer state and scoring.** `scripts/kalshi_advise/commodities_{15m,hourly,daily}_calibrator.py` fit and score per family; state file becomes `by_family: {KXGOLDH: {...}, ...}`. Per-family model, sample counts, Brier, eligible evidence, trust.
2. **One atomically written report** in the requested shape — `by_family.<FAMILY>.{model, evidence, trust}` — plus a `pooled` block retained **as diagnostics only**, explicitly marked non-authorizing.
3. **Bidding requests trust by family.** `KalshiBotDecision::signal_trusted(report)` becomes `signal_trusted(report, family)`, reading `by_family[family].trust`. **Absent family ⇒ refuse**, never fall back to pooled — same fail-closed shape as `permit()` in #232.
4. **Apply to every multi-family report**, not only commodities 15m, and give the three single-family reports an explicit family.
5. **Per-family UI** (row 1 = one box per preregistered family with `settled/300` and that family's verdict; row 2 = Brier/system, settlements, sealed gate, kill switch, exposure).
6. **Only then** preregister families in the sealed gate.

## Tests (the six required, plus two the measurements imply)

1. Gold PASS never admits Silver or WTI.
2. Pooled PASS never overrides a family FAIL.
3. Updating Gold's model cannot change Silver's model state.
4. Missing / unregistered family refuses.
5. Restart preserves each family's independent model and counters.
6. Per-family totals reconcile with the pooled diagnostic totals.
7. **A family with too few samples reports UNAVAILABLE, not false-and-therefore-safe** — the distinction matters because "no evidence" and "evidence of no edge" must not both silently read as "don't bid" while one of them later flips.
8. **The pooled block cannot be read by any authorizing path** — assert by construction (the authorizing function takes a family and has no pooled fallback), not by convention.

Each gets a neuter: the test must fail when the property is removed.

## What this does not fix

Splitting evidence three ways **shrinks every family's sample**. `commodities-hourly` has 500 pooled scored contracts; split, no family has 500. Against the sealed gate's 300-settled floor per family, per-family promotion gets materially slower — that is the honest cost of measuring the right thing, and it should be accepted rather than engineered around.

It also does not change the finding in #233: a family's edge must be *distinguishable from zero*, not merely positive. Splitting the evidence makes each family's interval wider, so the significance criterion will bite harder, not less.

## Sequencing against open work

PR #233 (significance criterion + drawdown noise floor) is independent and can merge on its own. It does not authorize pooled commodity evidence — it only changes what a future gate measures. Nothing in it should be read as making the pooled reports safe.
