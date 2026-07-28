#!/usr/bin/env python3
"""WHAT THE RECORD TEACHES — the recurring edge-autopsy artifact (issue #174).

The #169 autopsy produced decisive numbers and put them in a markdown report.
A report is a snapshot: it was true of the record on 2026-07-27 and says
nothing about the record today. Lessons that future bets depend on have to be
RECURRING, DATED and SAMPLE-SIZED, and they have to live where the bets are
made. This driver is what makes that possible: it runs #169's four question
scripts unchanged and reduces each one's (large, nested) output to ONE short
conclusion that the BOT tab, the cockpit and `kalshi bot lessons` render.

Three rules are structural here, not stylistic.

  1. **The verdict is DERIVED from this run's numbers, every run.** Nothing in
     this file hardcodes a conclusion from the 2026-07-27 snapshot. Each
     question's decision rule is a named constant block below, and each
     constant carries the report section whose stated criterion it encodes —
     so this is the autopsy's own reasoning applied to fresh numbers, not a new
     analysis and not a canned answer. If the record changes, the verdicts
     change with it. (A verdict copied from the report would be a canned number
     in the charter's sense: the highest-severity class of bug in this repo.)
  2. **The sample size is carried in the unit the question measures in, and it
     is never a row count.** Q2/Q3 count CONTRACTS, Q4 counts SETTLED
     POSITIONS, Q1 counts independent SPOT MOVES. Quoting Q2's 5,011 forecast
     rows or Q4's 87,801 decision rows as "the sample" would make a lesson from
     a dozen outcomes look like a lesson from tens of thousands, which is
     precisely the misreading this artifact exists to prevent. Forcing every
     question into "contracts" would be the same error wearing the right word:
     Q1 does not measure contracts, so it does not claim any.
  3. **A question that could not be measured says so.** A script that fails to
     run, times out, or emits unparseable output becomes an
     INSUFFICIENT_DATA lesson carrying the failure text. It never vanishes from
     the artifact, and it never inherits the previous run's conclusion.

READ-ONLY against the ledgers, exactly as #169 is: this driver spawns #169's
scripts and writes ONE file, the artifact itself, atomically (temp + rename, so
a failed refresh cannot leave a half-written report behind). The scripts it
spawns open evidence strictly for reading — including their deliberate refusal
to import `spot_calibrator`, whose `run_once()` would rewrite the operator's
live state as a side effect of reading it.

`--refresh` from `kalshi bot lessons` and the weekly launchd job
(scripts/deploy/org.openterminal.kalshi-edge-report.plist) both run THIS file,
so the scheduled artifact and the on-demand one cannot drift into two writers.

    python3 scripts/research/kalshi_edge_report.py            # publish
    python3 scripts/research/kalshi_edge_report.py --stdout   # print, write nothing
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import kalshi_edge_common as common  # noqa: E402

SCHEMA = 1
ARTIFACT = "kalshi-edge-report.json"

# --- the verdict vocabulary -------------------------------------------------
# One enum across all four questions. Not every value is reachable for every
# question — FEE_EATEN needs a cost dimension, which Q2 and Q3 (pure forecast
# scoring, no order) do not have — and a question that cannot reach a value
# simply never returns it. Inventing a cost for Q2 so the enum looks uniform
# would be fabricating the thing being measured.
EDGE = "EDGE"                          # measured, and it survives its own costs
NO_EDGE = "NO_EDGE"                    # measured, and there is nothing there
FEE_EATEN = "FEE_EATEN"                # real effect, smaller than the cost of taking it
INSUFFICIENT_DATA = "INSUFFICIENT_DATA"  # the record cannot answer the question yet
VERDICTS = (EDGE, NO_EDGE, FEE_EATEN, INSUFFICIENT_DATA)

# --- Q1's decision rule -----------------------------------------------------
# Report §"Q1 — Kalshi quote lag versus spot". Its verdict rests on three
# stated things: the 15-second horizon is where the drift is largest and
# clearest; significance is read off the t-statistic; and the drift is compared
# against the half-spread PLUS the fee, because "the FEE, not the spread, eats
# the slowness".
Q1_HORIZON_S = 15
Q1_MIN_T_STAT = 2.0
# The t-statistic is computed over per-(event, market) OBSERVATIONS, which are
# correlated inside one spot move — 44 observations at 3 sigma come from 8
# moves. So the independent unit is the event, and a mean over fewer than this
# many independent moves is not something to publish a verdict from; the report
# says as much where it calls 8 events "directionally consistent" and caveats
# it. Strata are considered from the HIGHEST sigma down, and the first one with
# enough independent events decides: a fixed, sample-size-only rule, decided
# before any outcome is read.
Q1_MIN_EVENTS = 10

# --- Q2's and Q3's decision rule --------------------------------------------
# The floor is the repo's own preregistered one, not a number chosen here:
# `spot_calibrator.MIN_SCORED_CONTRACTS` (issue #171) is how many resolved
# CONTRACTS the calibrator's Brier must cover before `adds_value_over_market`
# is allowed to mean anything. The same floor governs whether a Brier
# comparison is worth publishing as a lesson.
MIN_SCORED_CONTRACTS = 100

# --- Q4's decision rule -----------------------------------------------------
# The sealed promotion gate's own `min_settled_bids` decides when the bot's own
# record is answerable; it is read from the gate the run actually finds, and
# this constant is used only when the gate publishes none. Report §Q4: "Winners
# vs losers is not measurable at n = 15."
Q4_FALLBACK_MIN_SETTLED = 300

# --- the questions, and how long each is allowed to take ---------------------
# Q1 pairs 162k BRTI ticks against 126k two-sided quotes; it is the slow one.
QUESTIONS = (
    {"id": "Q1", "script": "q1_quote_lag.py", "timeout_s": 900,
     "title": "QUOTE LAG VS SPOT"},
    {"id": "Q2", "script": "q2_calibrator_error_anatomy.py", "timeout_s": 900,
     "title": "CALIBRATOR VS THE MARKET MID"},
    {"id": "Q3", "script": "q3_recalibration_headroom.py", "timeout_s": 900,
     "title": "RECALIBRATION HEADROOM"},
    {"id": "Q4", "script": "q4_bid_postmortem.py", "timeout_s": 600,
     "title": "THE BOT'S OWN SETTLED BIDS"},
)


# ── formatting helpers ──────────────────────────────────────────────────────
# Numbers are formatted HERE, once, and both renderers print the resulting
# text verbatim. The raw value travels beside it so a machine reader never has
# to parse a rendered string.

def _number(label, value, text):
    return {"label": label, "value": value, "text": text}


def _cents(dollars):
    """A probability-dollar difference in cents — the unit Q1 reports in."""
    return None if dollars is None else dollars * 100.0


def _cents_text(dollars, digits=2):
    if dollars is None:
        return "unmeasured"
    return "%+.*f¢" % (digits, _cents(dollars))


def _usd_text(dollars):
    return "unmeasured" if dollars is None else "%+.2f USD" % dollars


def _brier_text(value):
    return "unmeasured" if value is None else "%.4f" % value


def _hours_between(from_iso, to_iso):
    try:
        start = datetime.datetime.fromisoformat(from_iso)
        end = datetime.datetime.fromisoformat(to_iso)
    except (TypeError, ValueError):
        return None
    return (end - start).total_seconds() / 3600.0


def _span_text(hours):
    if hours is None:
        return "span unstated"
    return "%.1fh of record" % hours if hours < 48.0 else "%.1fd of record" % (hours / 24.0)


def _span(from_iso, to_iso, hours=None):
    """The window a lesson was measured over, or None when it is unstated.

    A span the payload did not carry is absent, never a zero: "measured over
    0 hours" is a claim, and it is a false one.
    """
    if not from_iso or not to_iso:
        return None
    if hours is None:
        hours = _hours_between(from_iso, to_iso)
    return {"from_utc": from_iso, "to_utc": to_iso, "hours": hours,
            "text": _span_text(hours)}


def _sample(n, unit, detail=None):
    return {"n": n, "unit": unit, "detail": detail}


def _lesson(question, verdict, claim, reason, key_numbers=None, sample=None, span=None):
    """One question's whole conclusion, in the shape both renderers read."""
    assert verdict in VERDICTS, verdict
    return {
        "id": question["id"],
        "title": question["title"],
        "verdict": verdict,
        "claim": claim,
        "verdict_reason": reason,
        "key_numbers": key_numbers or [],
        "sample": sample or _sample(None, "not stated"),
        "data_span": span,
        "command": "python3 scripts/research/%s" % question["script"],
    }


def _unmeasured(question, why):
    """A question whose script did not produce a payload this run.

    It stays in the artifact as INSUFFICIENT_DATA carrying the failure text. A
    question that silently disappeared would read on the card as a question
    nobody asked, and one that kept the previous run's conclusion would be the
    canned-number defect wearing a fresh timestamp.
    """
    return _lesson(question, INSUFFICIENT_DATA,
                   "not measured this run — the question's script produced no readable result",
                   why)


# ── Q1 ───────────────────────────────────────────────────────────────────────

def _q1_stratum(payload):
    """The stratum the rule selects, plus the horizon row inside it.

    Highest sigma first; the first stratum with at least Q1_MIN_EVENTS
    independent spot moves decides. Selection is on sample size alone and never
    on the outcome, so it cannot be a way of picking the answer.
    """
    strata = sorted(payload.get("by_threshold") or [],
                    key=lambda s: s.get("threshold_sigma") or 0.0, reverse=True)
    thin = []
    for stratum in strata:
        summary = stratum.get("contested_markets") or {}
        events = summary.get("events")
        horizon = None
        for row in summary.get("horizons") or []:
            if row.get("seconds") == Q1_HORIZON_S:
                horizon = row
        if events is None or horizon is None:
            thin.append(stratum)
            continue
        if events >= Q1_MIN_EVENTS:
            return stratum, summary, horizon, None
        thin.append(stratum)
    counts = ", ".join("%sσ: %s events" % (s.get("threshold_sigma"),
                                                (s.get("contested_markets") or {}).get("events"))
                       for s in thin) or "no stratum was measured at all"
    return None, None, None, counts


def reduce_q1(question, payload):
    stratum, summary, horizon, thin = _q1_stratum(payload)
    data = payload.get("data") or {}
    window = data.get("paired_window_utc") or [None, None]
    span = _span(window[0], window[1], data.get("paired_window_hours"))
    if stratum is None:
        return _lesson(
            question, INSUFFICIENT_DATA,
            "whether the Kalshi book lags spot cannot be answered on this record",
            "no volatility stratum reached %d independent spot moves at the %ds horizon (%s); "
            "the t-statistic is computed over observations that are correlated inside one move, "
            "so fewer independent moves than this is not a measurement"
            % (Q1_MIN_EVENTS, Q1_HORIZON_S, thin),
            sample=_sample(None, "spot moves"), span=span)

    sigma = stratum.get("threshold_sigma")
    drift = horizon.get("mean_aligned_mid_drift")
    t_stat = horizon.get("t_stat")
    net_taking = horizon.get("net_of_cost_taking")
    net_fee_only = horizon.get("net_of_fee_only")
    sample = _sample(summary.get("events"), "spot moves at %sσ" % sigma,
                     "%s aligned observations across %s markets"
                     % (horizon.get("n"), summary.get("markets")))
    numbers = [
        _number("mid drift @%ds" % Q1_HORIZON_S, drift, _cents_text(drift)),
        _number("t", t_stat, "unmeasured" if t_stat is None else "%.2f" % t_stat),
        _number("half-spread", horizon.get("mean_half_spread"),
                _cents_text(horizon.get("mean_half_spread")).lstrip("+")),
        _number("fee", horizon.get("mean_fee"),
                _cents_text(horizon.get("mean_fee")).lstrip("+")),
        _number("net if taking", net_taking, _cents_text(net_taking)),
        _number("net vs fee only", net_fee_only, _cents_text(net_fee_only)),
    ]

    if drift is None or t_stat is None:
        return _lesson(question, INSUFFICIENT_DATA,
                       "the drift after a spot move could not be scored on this record",
                       "the stratum carries no mean drift or no standard error to test it against",
                       numbers, sample, span)
    if t_stat < Q1_MIN_T_STAT:
        return _lesson(
            question, NO_EDGE,
            "the Kalshi book does not measurably keep moving after a spot move it already knows "
            "about",
            "t %.2f at the %ds horizon is below %.1f, so the drift is not distinguishable from "
            "zero — cost never enters the question" % (t_stat, Q1_HORIZON_S, Q1_MIN_T_STAT),
            numbers, sample, span)
    if drift <= 0.0:
        return _lesson(
            question, NO_EDGE,
            "the Kalshi book moves AGAINST the completed spot move, not with it — there is no lag "
            "to harvest here",
            "mean drift %s at t %.2f: significant, and in the wrong direction for a lag"
            % (_cents_text(drift), t_stat), numbers, sample, span)
    if net_taking is not None and net_taking > 0.0:
        return _lesson(
            question, EDGE,
            "the Kalshi book lags spot by more than it costs to take the quote",
            "mean drift %s at t %.2f clears half-spread + fee with %s left over"
            % (_cents_text(drift), t_stat, _cents_text(net_taking)), numbers, sample, span)
    return _lesson(
        question, FEE_EATEN,
        "the lag is REAL and it is almost exactly fee-sized: the book is slow, and the cost of "
        "taking it eats the slowness",
        "mean drift %s at t %.2f, against half-spread %s + fee %s, leaves %s to a taker%s"
        % (_cents_text(drift), t_stat,
           _cents_text(horizon.get("mean_half_spread")).lstrip("+"),
           _cents_text(horizon.get("mean_fee")).lstrip("+"), _cents_text(net_taking),
           "" if net_fee_only is None or net_fee_only <= 0.0
           else " — it survives the FEE alone (%s), so the follow-up is capturing it without "
                "crossing" % _cents_text(net_fee_only)),
        numbers, sample, span)


# ── Q2 ───────────────────────────────────────────────────────────────────────

def reduce_q2(question, payload):
    overall = payload.get("overall") or {}
    contracts = overall.get("contracts")
    span_utc = payload.get("contract_span_utc") or [None, None]
    span = _span(span_utc[0], span_utc[1])
    calibrated = overall.get("brier_calibrated")
    market = overall.get("brier_market_mid")
    delta = overall.get("delta_vs_market")
    sample = _sample(contracts, "settled contracts",
                     None if overall.get("rows") is None
                     else "%s forecast rows, scored per contract" % overall.get("rows"))
    numbers = [
        _number("calibrator Brier", calibrated, _brier_text(calibrated)),
        _number("raw market mid", market, _brier_text(market)),
        _number("delta vs mid", delta,
                "unmeasured" if delta is None else "%+.4f" % delta),
    ]
    if contracts is None or contracts < MIN_SCORED_CONTRACTS:
        return _lesson(
            question, INSUFFICIENT_DATA,
            "whether the calibrator beats the raw market mid is not yet answerable",
            "%s settled contracts is below the %d the calibrator's own gate requires before its "
            "Brier is allowed to mean anything (spot_calibrator.MIN_SCORED_CONTRACTS)"
            % ("no" if contracts is None else contracts, MIN_SCORED_CONTRACTS),
            numbers, sample, span)
    if calibrated is None or market is None:
        return _lesson(question, INSUFFICIENT_DATA,
                       "the calibrator and the market mid could not both be scored on this record",
                       "one of the two Brier scores is unavailable, so no comparison is claimed",
                       numbers, sample, span)
    if calibrated < market:
        return _lesson(
            question, EDGE,
            "the calibrator's probabilities are better than the raw market mid",
            "Brier %s versus the mid's %s over %s contracts — lower is better, so the signal "
            "carries information the price does not"
            % (_brier_text(calibrated), _brier_text(market), contracts),
            numbers, sample, span)
    return _lesson(
        question, NO_EDGE,
        "the calibrator is not mistuned, it is uninformative: the raw market mid scores better",
        "Brier %s versus the mid's %s over %s contracts (%+.4f) — the signal is worse than the "
        "price it is trying to beat"
        % (_brier_text(calibrated), _brier_text(market), contracts, delta),
        numbers, sample, span)


# ── Q3 ───────────────────────────────────────────────────────────────────────

def reduce_q3(question, payload):
    span_utc = payload.get("contract_span_utc") or [None, None]
    span = _span(span_utc[0], span_utc[1])
    aggregate = ((payload.get("walk_forward") or {}).get("aggregate")
                 if isinstance(payload.get("walk_forward"), dict) else None) or {}
    contracts = aggregate.get("pooled_test_contracts")
    platt = aggregate.get("platt_gain_vs_raw")
    isotonic = aggregate.get("isotonic_gain_vs_raw")
    sample = _sample(contracts, "out-of-sample contracts",
                     None if not aggregate.get("folds")
                     else "%s walk-forward folds, contracts never split by row"
                          % aggregate.get("folds"))
    numbers = [
        _number("Platt gain", platt, "unmeasured" if platt is None else "%+.4f" % platt),
        _number("isotonic gain", isotonic,
                "unmeasured" if isotonic is None else "%+.4f" % isotonic),
        _number("corrected vs mid", aggregate.get("best_corrected_vs_market"),
                "unmeasured" if aggregate.get("best_corrected_vs_market") is None
                else "%+.4f" % aggregate.get("best_corrected_vs_market")),
    ]
    if contracts is None or contracts < MIN_SCORED_CONTRACTS or platt is None or isotonic is None:
        return _lesson(
            question, INSUFFICIENT_DATA,
            "whether recalibrating the existing signal would help is not yet answerable",
            "the walk-forward covers %s out-of-sample contracts against a floor of %d "
            "(spot_calibrator.MIN_SCORED_CONTRACTS)%s"
            % ("no" if contracts is None else contracts, MIN_SCORED_CONTRACTS,
               "" if platt is not None and isotonic is not None
               else "; and at least one corrector could not be scored"),
            numbers, sample, span)
    # Two conditions, and BOTH are required. A corrector that improves the
    # calibrator on its own terms while the corrected score is still worse than
    # the raw market mid has found no edge — it has moved a losing signal
    # slightly less far from the price. The report says exactly this where a
    # fully overfitted in-sample fit "merely ties the market price"; reporting
    # the first condition alone would publish EDGE for a signal that still
    # cannot beat the number it is bidding against.
    best = max(platt, isotonic)
    versus_market = aggregate.get("best_corrected_vs_market")   # negative = beats the mid
    if best > 0.0 and versus_market is not None and versus_market < 0.0:
        return _lesson(
            question, EDGE,
            "recalibrating the existing signal improves it out of sample AND carries it past the "
            "market mid",
            "the better corrector gains %+.4f Brier on walk-forward folds over %s out-of-sample "
            "contracts (Platt %+.4f / isotonic %+.4f) and scores %+.4f against the mid"
            % (best, contracts, platt, isotonic, versus_market), numbers, sample, span)
    if best > 0.0:
        return _lesson(
            question, NO_EDGE,
            "correction moves the signal but does not carry it past the market mid — there is no "
            "headroom worth trading",
            "the better corrector gains %+.4f Brier out of sample (Platt %+.4f / isotonic %+.4f) "
            "over %s contracts, yet the corrected score is still %s the raw mid"
            % (best, platt, isotonic, contracts,
               "unmeasured against" if versus_market is None
               else "%+.4f worse than" % versus_market), numbers, sample, span)
    return _lesson(
        question, NO_EDGE,
        "there is no honest recalibration headroom — post-hoc correction makes the signal WORSE "
        "out of sample",
        "walk-forward Platt %+.4f and isotonic %+.4f (negative is worse) over %s out-of-sample "
        "contracts; an in-sample fit can drive this arbitrarily low and is not evidence"
        % (platt, isotonic, contracts), numbers, sample, span)


# ── Q4 ───────────────────────────────────────────────────────────────────────

def _q4_required_settled(payload):
    """The sealed gate's own minimum, or the fallback when it publishes none."""
    gate = payload.get("gate") or {}
    params = gate.get("params") or {}
    required = params.get("min_settled_bids")
    if isinstance(required, (int, float)) and required > 0:
        return int(required), "the sealed gate's own min_settled_bids"
    return Q4_FALLBACK_MIN_SETTLED, ("no sealed gate published a min_settled_bids, so the "
                                     "charter's %d is used" % Q4_FALLBACK_MIN_SETTLED)


def reduce_q4(question, payload):
    summary = payload.get("outcome_summary") or {}
    data = payload.get("data") or {}
    span_utc = data.get("span_utc") or [None, None]
    span = _span(span_utc[0], span_utc[1])
    settled = summary.get("settled")
    net = summary.get("net_realized_pnl_usd")
    fees = summary.get("fees_usd")
    wins, losses = summary.get("wins"), summary.get("losses")
    required, required_source = _q4_required_settled(payload)
    sample = _sample(settled, "settled positions",
                     None if wins is None or losses is None
                     else "%s won / %s lost" % (wins, losses))
    numbers = [
        _number("net after fees", net, _usd_text(net)),
        _number("fees", fees, "unmeasured" if fees is None else "%.2f USD" % fees),
        _number("win rate", summary.get("win_rate"),
                "unmeasured" if summary.get("win_rate") is None
                else "%.0f%%" % (summary["win_rate"] * 100.0)),
    ]
    if settled is None or settled < required:
        return _lesson(
            question, INSUFFICIENT_DATA,
            "the bot's own record is too short to teach anything about which bids work",
            "%s settled positions against the %d required (%s): any split of a sample this size "
            "into winners and losers produces groups whose rates differ by chance alone%s"
            % ("no" if settled is None else settled, required, required_source,
               "" if net is None else ". What IS measured: %s net after %s fees"
               % (_usd_text(net), "unmeasured" if fees is None else "%.2f USD" % fees)),
            numbers, sample, span)
    if net is None:
        return _lesson(question, INSUFFICIENT_DATA,
                       "the bot's settled record carries no realized P&L to score",
                       "the settlement rows published no net realized P&L",
                       numbers, sample, span)
    if net > 0.0:
        return _lesson(
            question, EDGE,
            "the bot's own settled bids make money after fees",
            "%s over %s settled positions, fees %s included"
            % (_usd_text(net), settled, "unmeasured" if fees is None else "%.2f USD" % fees),
            numbers, sample, span)
    gross = None if fees is None else net + fees
    if gross is not None and gross > 0.0:
        return _lesson(
            question, FEE_EATEN,
            "the bot's own settled bids win before fees and lose after them",
            "%s gross over %s settled positions becomes %s once %.2f USD of fees are paid"
            % (_usd_text(gross), settled, _usd_text(net), fees),
            numbers, sample, span)
    return _lesson(
        question, NO_EDGE,
        "the bot's own settled bids lose money, before fees as well as after",
        "%s over %s settled positions" % (_usd_text(net), settled),
        numbers, sample, span)


REDUCERS = {"Q1": reduce_q1, "Q2": reduce_q2, "Q3": reduce_q3, "Q4": reduce_q4}


# ── the pure reduction ──────────────────────────────────────────────────────

def reduce(results, generated_at_ms):
    """(question id -> run result) -> the artifact. PURE: no I/O, no clock.

    `results` maps each question id to either {"payload": <the script's JSON>}
    or {"error": "<why it produced none>"}. Every question in QUESTIONS appears
    in the output exactly once, in order, whichever it was.
    """
    lessons = []
    for question in QUESTIONS:
        result = results.get(question["id"]) or {}
        payload = result.get("payload")
        if not isinstance(payload, dict):
            lessons.append(_unmeasured(question, result.get("error")
                                       or "the question was not run"))
            continue
        try:
            lessons.append(REDUCERS[question["id"]](question, payload))
        except (AttributeError, IndexError, KeyError, TypeError, ValueError,
                ZeroDivisionError) as exc:
            # A payload shaped differently than this reducer expects is a
            # question that cannot be read, not a question with no edge.
            lessons.append(_unmeasured(
                question, "the script's output could not be reduced (%s: %s)"
                          % (type(exc).__name__, exc)))
    return {
        "schema": SCHEMA,
        "event": "kalshi_edge_report",
        "generated_at": datetime.datetime.fromtimestamp(
            generated_at_ms / 1000.0, datetime.timezone.utc).isoformat(),
        "generated_at_ms": generated_at_ms,
        "source": ("scripts/research/kalshi_edge_report.py over issue #169's q1-q4 scripts, "
                   "read-only against the evidence ledgers"),
        "verdicts": list(VERDICTS),
        "lessons": lessons,
    }


# ── running the questions ───────────────────────────────────────────────────

def run_question(question, python=None):
    """One question's script, as a subprocess. Never raises.

    Per-question isolation is the point: Q1 timing out must not cost the other
    three their conclusions. stdout is parsed REGARDLESS of exit code, because
    q2 exits 1 on its no-data path while still emitting a full payload — an
    exit-code gate would turn a stated absence into an unexplained failure.
    """
    path = os.path.join(_HERE, question["script"])
    argv = [python or sys.executable, path]
    try:
        done = subprocess.run(argv, cwd=_HERE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=question["timeout_s"])
    except subprocess.TimeoutExpired:
        return {"error": "%s did not finish within %ds"
                         % (question["script"], question["timeout_s"])}
    except OSError as exc:
        return {"error": "%s could not be run (%s)" % (question["script"], exc)}
    try:
        return {"payload": json.loads(done.stdout.decode("utf-8", "replace"))}
    except ValueError:
        tail = done.stderr.decode("utf-8", "replace").strip().splitlines()
        return {"error": "%s printed no parseable JSON (exit %d)%s"
                         % (question["script"], done.returncode,
                            (": " + tail[-1]) if tail else "")}


def publish(report, out_path=None):
    """Atomically write the artifact. Temp + rename, so a refresh that dies
    half-way leaves the previous artifact intact rather than a truncated one
    that reads as a fresh report."""
    out_path = out_path or common.evidence_path(ARTIFACT)
    tmp_path = out_path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
    os.replace(tmp_path, out_path)
    return out_path


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stdout", action="store_true",
                        help="print the artifact instead of publishing it")
    parser.add_argument("--python", help="interpreter used for the question scripts")
    parser.add_argument("--out", help="write the artifact here instead of the evidence dir")
    args = parser.parse_args()

    results = {}
    for question in QUESTIONS:
        sys.stderr.write("kalshi-edge-report: running %s (%s)\n"
                         % (question["id"], question["script"]))
        sys.stderr.flush()
        results[question["id"]] = run_question(question, args.python)
    # The clock is read HERE and nowhere else — `reduce()` is pure so its
    # verdict mapping is testable without one.
    report = reduce(results, int(time.time() * 1000))

    if args.stdout:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    try:
        path = publish(report, args.out)
    except OSError as exc:
        sys.stderr.write("kalshi-edge-report: cannot write the artifact (%s)\n" % exc)
        return 1
    print(json.dumps({"published": path,
                      "verdicts": {l["id"]: l["verdict"] for l in report["lessons"]}}))
    # A refresh in which every question failed is not a successful refresh,
    # even though it wrote a well-formed artifact saying so.
    return 0 if any(l["verdict"] != INSUFFICIENT_DATA for l in report["lessons"]) else 3


if __name__ == "__main__":
    sys.exit(main())
