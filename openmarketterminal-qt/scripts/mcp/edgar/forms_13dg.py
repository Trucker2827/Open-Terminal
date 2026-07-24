"""
SC 13D / 13G Major-Holder Extraction
=====================================

Extract >5% beneficial owners of a company from the SC 13D / SC 13G
filings (and their amendments) in the company's OWN filing stream.

This is deliberately different from the 13F module: 13F-HR is
filer-centric (holdings *of* a fund), so it can never answer "who holds
this operating company". Schedules 13D/G are filed *about* the issuer
and therefore appear in the issuer's stream — each names a reporting
person (the beneficial owner), and structured (2025+) filings carry
shares and percent-of-class.

Honesty contract: older text-only filings parse to a real owner name and
filing provenance but no reliable numbers — those are reported as None,
never as fabricated zeros. Structured filings report the filed values
verbatim (a 0.0% amendment is a real exit, and is kept as 0.0).
"""

from typing import Any, Dict, List
import traceback

try:
    from edgar import Company
    EDGAR_AVAILABLE = True
except ImportError:
    EDGAR_AVAILABLE = False
    Company = None  # patched in tests; guarded by check_edgar_available()

from .base import EdgarError, check_edgar_available

# All historical + current form labels for Schedules 13D and 13G.
FORMS_13DG = [
    "SC 13D", "SC 13G", "SC 13D/A", "SC 13G/A",
    "SCHEDULE 13D", "SCHEDULE 13G", "SCHEDULE 13D/A", "SCHEDULE 13G/A",
]


def _num(x):
    """Coerce numpy/None/str to a plain float, or None (drops NaN)."""
    if x is None:
        return None
    try:
        f = float(x)
        return f if f == f else None
    except (TypeError, ValueError):
        return None


def _norm_name(name: str) -> str:
    """Dedup key for a reporting person: case/whitespace-insensitive."""
    return " ".join(str(name).upper().split())


def aggregate_reporting_persons(rows: List[Dict[str, Any]], limit: int) -> List[Dict[str, Any]]:
    """
    Collapse per-filing reporting-person rows (newest first) into one row
    per distinct holder, keeping the newest filing for each. Pure — no
    network — so the dedup/provenance contract is unit-testable.
    """
    seen = set()
    holders = []
    for row in rows:
        name = row.get("name")
        if not name:
            continue
        key = _norm_name(name)
        if key in seen:
            continue
        seen.add(key)
        holders.append(row)
        if len(holders) >= limit:
            break
    return holders


def get_major_holders(ticker: str, limit: int = 10, max_scan: int = 25) -> Dict[str, Any]:
    """
    Get >5% beneficial owners from SC 13D/G filings in the company's own
    filing stream.

    One row per distinct reporting person (newest filing wins): name,
    percent_of_class, shares, person_type, form, filing_date,
    is_amendment. percent/shares are None when the filing is a legacy
    text-only document without structured ownership data.

    Contract (mirrors forms_insider): if 13D/G filings exist but zero
    reporting persons parse, that is a parser failure -> {"error": ...},
    never a silent empty success. Only a company with NO 13D/G filings
    returns an empty success.
    """
    try:
        check_edgar_available()
        company = Company(ticker)
        filings = company.get_filings(form=FORMS_13DG)

        n_filings = 0 if not filings else len(filings)
        if n_filings == 0:
            return {"success": True, "ticker": ticker.upper(), "data": [], "count": 0, "scanned": 0}

        rows = []
        scanned = 0
        for filing in filings:
            if scanned >= max_scan:
                break
            scanned += 1
            try:
                sched = filing.obj()
                if sched is None:
                    continue
                structured = bool(getattr(sched, "has_structured_data", False))
                persons = getattr(sched, "reporting_persons", None) or []
                for p in persons:
                    name = getattr(p, "name", None)
                    if not name:
                        continue
                    rows.append({
                        "name": str(name),
                        "percent_of_class": _num(getattr(p, "percent_of_class", None)) if structured else None,
                        "shares": _num(getattr(p, "aggregate_amount", None)) if structured else None,
                        "person_type": getattr(p, "type_of_reporting_person", None) or None,
                        "form": str(filing.form),
                        "filing_date": str(filing.filing_date),
                        "is_amendment": "/A" in str(filing.form),
                    })
            except Exception:
                continue  # skip an unparseable filing; the no-rows guard below still applies

        if not rows:
            return {"error": EdgarError(
                "get_major_holders",
                f"parsed 0 reporting persons from {scanned} SC 13D/G filing(s) for {ticker} — "
                "the Schedule 13D/G parser may be out of date").to_dict()}

        holders = aggregate_reporting_persons(rows, limit)
        return {
            "success": True,
            "ticker": ticker.upper(),
            "data": holders,
            "count": len(holders),
            "scanned": scanned,
        }
    except Exception as e:
        return {"error": EdgarError("get_major_holders", str(e), traceback.format_exc()).to_dict()}
