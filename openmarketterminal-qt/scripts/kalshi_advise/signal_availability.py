#!/usr/bin/env python3
"""MEASUREMENT ONLY. Separate "measured neutral" from "never wired".

Today a signal cannot express the difference. Every ensemble slot collapses
both states onto one number:

    result["trade_flow"] = 0.0      # overridden in observe_cycle
    result["spot_drift"] = 0.0
    result["news_forecast"] = 0.0
    result["event_pressure"] = 0.0

and `spot_calibrator.news_forecast_feature` says so in its own docstring: 0.0
when the source is "None, unreadable, or the field is absent". Four distinct
states, one value.

`book_imbalance` has the same defect and it is NOT a stub -- it is live in
production:

    result["book_imbalance"] = (bid_sz - ask_sz) / denom if denom > 0.0 else 0.0

`denom == 0` means there is NO BOOK. It returns the same 0.0 as a perfectly
balanced one, so the model can never learn "the book was empty, distrust this
quote" -- it never sees that the book was empty.

Zero is a legitimate observation. Missing data is a different fact.

A NON-ZERO RATE IS NOT AN AVAILABILITY RATE
-------------------------------------------
This module deliberately does NOT infer availability from how often a stored
value is zero. That inference is unsound in both directions: a measured neutral
is legitimately zero, and a stored zero cannot retrospectively be separated
into

    source unavailable | source stale | available but empty | available, neutral

Rows already written carry no availability metadata, so for historical data the
honest answer is "unknown". What CAN be established is:

  * the SOURCE's state right now (present, readable, age)
  * the CODE PATH, which for commodities never reads the aux sources at all

Both are reported. Zero counts appear only under `zero_rate`, labelled as an
observation about values and explicitly not as an availability figure.

THE CONTRACT THIS ARGUES FOR
----------------------------
Each signal should carry six fields, so the states are separable at write time
rather than guessed at read time:

    available, value, observed_at, source, source_age_ms, reason_unavailable

`describe()` below emits exactly that shape. Nothing consumes it yet: adopting
it changes model inputs and therefore behaviour, which belongs in its own
change behind a schema/model version bump -- not in a measurement.

AND NOT NaN
-----------
An unavailable signal must never reach the learner as NaN. The eventual form is
an imputed neutral numeric value PLUS an explicit availability indicator, so
the model can condition on "this was missing" instead of silently training on a
number that means nothing.

PROVENANCE
----------
The five are not equally independent of what they predict. `book_imbalance`,
`trade_flow` and `spot_drift` come from the SAME price formation that produces
the quote -- they may carry incremental timing information, but they are not
new information about the world. `news_forecast` and `event_pressure` are
genuinely external. A residual model beating the market on price-formation
signals alone is a weaker claim than one doing it with external signals, so the
class travels with the value.
"""
from __future__ import annotations

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import spot_calibrator as sc

PRICE_FORMATION = "price_formation"
EXTERNAL = "external"

# The six fields a signal must carry for the states to be separable.
CONTRACT_FIELDS = ("available", "value", "observed_at", "source",
                   "source_age_ms", "reason_unavailable")

# A source older than this is present but not usable as a current read. Stale
# is its own state: it is not "missing", and it is certainly not "neutral".
DEFAULT_STALE_MS = 15 * 60 * 1000

SIGNALS = {
    "book_imbalance": {
        "provenance": PRICE_FORMATION,
        "source": "daemon top-of-book sizes (snapshot.execution.yes)",
        "path": None,
        "zero_conflates": ["balanced book", "NO BOOK AT ALL (denom == 0)"],
    },
    "trade_flow": {
        "provenance": PRICE_FORMATION,
        "source": "kalshi-trades.jsonl tape",
        "path": sc.TRADES_PATH,
        "zero_conflates": ["no net flow", "tape missing", "tape stale"],
    },
    "spot_drift": {
        "provenance": PRICE_FORMATION,
        "source": "btc-intelligence.jsonl spot series",
        "path": sc.SPOT_SERIES_PATH,
        "zero_conflates": ["no drift", "series missing", "only one sample"],
    },
    "news_forecast": {
        "provenance": EXTERNAL,
        "source": "btc-intelligence-latest.json news_context.score",
        "path": sc.INTEL_LATEST_PATH,
        "zero_conflates": ["neutral news", "file missing", "unreadable", "field absent"],
    },
    "event_pressure": {
        "provenance": EXTERNAL,
        "source": "btc-event-impact-latest.json",
        "path": sc.EVENT_IMPACT_LATEST_PATH,
        "zero_conflates": ["no scheduled pressure", "file missing", "unreadable"],
    },
}

# Producers that never read the aux sources at all. For these the code path --
# not any table of values -- is what establishes non-availability.
PRODUCERS_WITHOUT_AUX = ("strike_threshold_family",)


def describe(name, value=None, observed_at=None, now_ms=None, stale_ms=DEFAULT_STALE_MS):
    """One signal in the six-field contract.

    `available` is decided by the SOURCE, never by whether `value` is zero. A
    healthy source reporting 0.0 is available and neutral; a missing source is
    unavailable and its value is meaningless.
    """
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    spec = SIGNALS.get(name)
    if spec is None:
        return {"available": False, "value": None, "observed_at": None,
                "source": None, "source_age_ms": None,
                "reason_unavailable": "unknown signal"}
    state = probe_source(spec["path"], now_ms=now_ms, stale_ms=stale_ms)
    return {
        "available": state["available"],
        # A value is only meaningful when the source was available. Reporting a
        # number beside "unavailable" invites exactly the confusion this exists
        # to remove.
        "value": value if state["available"] else None,
        "observed_at": observed_at,
        "source": spec["source"],
        "source_age_ms": state.get("age_ms"),
        "reason_unavailable": None if state["available"] else state.get("reason"),
        "provenance": spec["provenance"],
    }


def probe_source(path, now_ms=None, stale_ms=DEFAULT_STALE_MS):
    """Present, readable, and how old -- with stale as its own state."""
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    if not path:
        return {"available": True, "age_ms": 0, "path": None}
    if not os.path.exists(path):
        return {"available": False, "reason": "missing", "path": path, "age_ms": None}
    try:
        age_ms = int(now_ms - os.path.getmtime(path) * 1000)
        size = os.path.getsize(path)
    except OSError:
        return {"available": False, "reason": "unreadable", "path": path, "age_ms": None}
    if size == 0:
        return {"available": False, "reason": "empty", "path": path, "age_ms": age_ms}
    if age_ms > stale_ms:
        return {"available": False, "reason": "stale", "path": path, "age_ms": age_ms}
    return {"available": True, "age_ms": age_ms, "path": path, "size_bytes": size}


def zero_rate(records, feature):
    """How often a stored value is exactly 0.0.

    NOT an availability figure, and labelled so wherever it appears. It says
    something about VALUES; historical rows carry no availability metadata, so
    what those zeros mean cannot be recovered from them.
    """
    total = zeros = 0
    for record in records:
        for obs in (record.get("observations") or []):
            value = obs.get(feature)
            if not isinstance(value, (int, float)):
                continue
            total += 1
            zeros += 1 if value == 0.0 else 0
    if total == 0:
        return None
    return {"observations": total, "exactly_zero": zeros,
            "zero_fraction": zeros / total,
            "means": "a value observation only — availability is NOT inferable from this"}


def build_report(now_ms=None, stale_ms=DEFAULT_STALE_MS):
    from openterminal_paths import evidence_file
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)

    sources = {}
    for name, spec in sorted(SIGNALS.items()):
        state = probe_source(spec["path"], now_ms=now_ms, stale_ms=stale_ms)
        sources[name] = {
            "provenance": spec["provenance"],
            "source": spec["source"],
            "available": state["available"],
            "source_age_ms": state.get("age_ms"),
            "reason_unavailable": None if state["available"] else state.get("reason"),
            "zero_conflates": spec["zero_conflates"],
        }

    families = {}
    for path, producer in (
        (evidence_file("commodities-hourly-calibrator-state.json"), "strike_threshold_family"),
        (evidence_file("commodities-15m-calibrator-state.json"), "commodities_15m_calibrator"),
        (evidence_file("spot-calibrator-state.json"), "spot_calibrator"),
    ):
        try:
            state = json.load(open(path, "r", encoding="utf-8"))
        except (OSError, ValueError):
            continue
        slices = state.get("by_family")
        if not isinstance(slices, dict):
            slices = {os.path.basename(path): state}
        for family, slice_state in sorted(slices.items()):
            records = slice_state.get("resolved_record") or []
            if not records:
                continue
            reads_aux = producer not in PRODUCERS_WITHOUT_AUX
            families[family] = {
                "producer": producer,
                "resolved_contracts": len(records),
                # THE code-path fact. For a producer that never reads the aux
                # sources, non-availability is established here -- not by any
                # table of values.
                "producer_reads_aux_sources": reads_aux,
                "signals_unavailable_by_code_path": (
                    [] if reads_aux else sorted(n for n, s in SIGNALS.items() if s["path"])),
                "zero_rate": {n: zero_rate(records, n) for n in sorted(SIGNALS)},
            }

    return {
        "event": "signal_availability",
        "advisory_only": True,
        "analysis_only": True,
        "generated_at_ms": now_ms,
        "contract_fields": list(CONTRACT_FIELDS),
        "problem": ("a signal returning 0.0 when its source is missing cannot be distinguished "
                    "from one measuring a genuine neutral"),
        "terminology": ("a non-zero rate is NOT an availability rate; historical rows carry no "
                        "availability metadata, so unavailable / stale / empty / neutral cannot "
                        "be separated retrospectively"),
        "never_nan": ("an unavailable signal must reach the learner as an imputed neutral value "
                      "PLUS an explicit availability indicator, never as NaN"),
        "sources": sources,
        "families": families,
        "note": ("MEASUREMENT ONLY. No feature, model, admission, sizing or sealed-parameter "
                 "change. Adopting the six-field contract alters model inputs and therefore "
                 "behaviour, and belongs in its own change behind a schema/model version bump."),
    }


def main(argv=None):
    print(json.dumps(build_report(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
