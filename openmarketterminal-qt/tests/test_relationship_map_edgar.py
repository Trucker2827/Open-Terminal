"""
Issue #83 — Relationship Map EDGAR rewire.

Covers the rewired (yfinance-free) sections of scripts/relationship_map.py
and the new SC 13D/G major-holders module (scripts/mcp/edgar/forms_13dg.py):

1. us-gaap concept mapping over edgar_get_financials output (consolidated
   rows only; derived EBITDA/FCF; missing concepts stay missing).
2. Insider ring built from edgar_insider_transactions rows (dedupe by
   person, buy/sell direction mapping).
3. SC 13D/G major-holder aggregation (newest filing per holder wins,
   legacy text-only filings keep provenance but no fabricated numbers,
   filings-but-zero-persons is a parser error, not an empty success).
4. The rewired sections never import yfinance (blocked via sys.modules).

Stdlib-only (unittest + fakes) — runs without edgartools/yfinance/network.
"""

import os
import sys
import types
import unittest

SCRIPTS_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, SCRIPTS_DIR)

# The rewired sections must work with yfinance entirely absent. Installing
# None makes any `import yfinance` raise ImportError immediately.
sys.modules["yfinance"] = None

import relationship_map  # noqa: E402
from mcp.edgar import forms_13dg  # noqa: E402


# ── Fixtures ────────────────────────────────────────────────────────────────

def _statement(concepts, values, dimensions=None, abstracts=None,
               period="2025-09-27 (FY)"):
    n = len(concepts)
    return {
        "concept": dict(enumerate(concepts)),
        "label": {i: f"label{i}" for i in range(n)},
        "dimension": dict(enumerate(dimensions or [None] * n)),
        "abstract": dict(enumerate(abstracts or [False] * n)),
        period: dict(enumerate(values)),
        # An older period that must be ignored by latest-period selection.
        "2024-09-28 (FY)": {i: -1.0 for i in range(n)},
    }


FIN_FIXTURE = {
    "success": True,
    "data": {
        "income_statement": {"data": _statement(
            ["us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
             "us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
             "us-gaap_OperatingIncomeLoss"],
            [416.0, 307.0, 133.0],
            dimensions=[None, "srt_ProductOrServiceAxis", None])},
        "cash_flow": {"data": _statement(
            ["us-gaap_DepreciationDepletionAndAmortization",
             "us-gaap_NetCashProvidedByUsedInOperatingActivities",
             "us-gaap_PaymentsToAcquirePropertyPlantAndEquipment"],
            [12.0, 110.0, 13.0])},
        "balance_sheet": {"data": _statement(
            ["us-gaap_CashAndCashEquivalentsAtCarryingValue",
             "us-gaap_MarketableSecuritiesCurrent",
             "us-gaap_LongTermDebtNoncurrent",
             "us-gaap_LongTermDebtCurrent",
             "us-gaap_CommercialPaper"],
            [30.0, 25.0, 78.0, 12.0, 8.0],
            period="2025-09-27")},
    },
}


INSIDER_ROWS = [
    {"filing_date": "2026-07-01", "insider": "Tim Cook", "position": "CEO",
     "transaction_type": "Sale", "code": "S", "shares": 100.0},
    {"filing_date": "2026-06-20", "insider": "TIM COOK", "position": "CEO",
     "transaction_type": "Purchase", "code": "P", "shares": 50.0},
    {"filing_date": "2026-06-01", "insider": "Luca Maestri", "position": "CFO",
     "transaction_type": None, "code": "M", "shares": 10.0},
    {"filing_date": "2026-05-15", "insider": None, "position": "Director",
     "transaction_type": "Sale", "code": "S", "shares": 5.0},
]


class FakePerson:
    def __init__(self, name, pct=None, shares=None, ptype="IA"):
        self.name = name
        self.percent_of_class = pct
        self.aggregate_amount = shares
        self.type_of_reporting_person = ptype


class FakeSchedule:
    def __init__(self, persons, structured=True):
        self.reporting_persons = persons
        self.has_structured_data = structured


class FakeFiling:
    def __init__(self, form, filing_date, sched):
        self.form = form
        self.filing_date = filing_date
        self._sched = sched

    def obj(self):
        if isinstance(self._sched, Exception):
            raise self._sched
        return self._sched


class FakeCompany:
    filings = []

    def __init__(self, ticker):
        self.ticker = ticker

    def get_filings(self, form=None):
        return list(self.filings)


# ── 1. Financials: us-gaap concept mapping ──────────────────────────────────

class TestFinancialsConceptMapping(unittest.TestCase):
    def test_flat_fields_from_statements(self):
        out = relationship_map.extract_financials_from_statements(FIN_FIXTURE)
        self.assertEqual(out["revenue"], 416.0)          # consolidated row, not the 307 segment row
        self.assertEqual(out["ebitda"], 133.0 + 12.0)    # operating income + D&A
        self.assertEqual(out["operating_cashflow"], 110.0)
        self.assertEqual(out["free_cashflow"], 110.0 - 13.0)
        self.assertEqual(out["total_cash"], 30.0 + 25.0)  # cash + short-term securities
        self.assertEqual(out["total_debt"], 78.0 + 12.0 + 8.0)
        self.assertEqual(out["period"], "2025-09-27")

    def test_dimension_rows_are_skipped(self):
        # Segment breakdown listed FIRST — the consolidated row must still win.
        fix = {"success": True, "data": {"income_statement": {"data": _statement(
            ["us-gaap_Revenues", "us-gaap_Revenues"],
            [307.0, 416.0],
            dimensions=["srt_ProductOrServiceAxis", None])}}}
        out = relationship_map.extract_financials_from_statements(fix)
        self.assertEqual(out["revenue"], 416.0)

    def test_missing_concepts_stay_missing(self):
        out = relationship_map.extract_financials_from_statements(
            {"success": True, "data": {}})
        for key in ("revenue", "ebitda", "operating_cashflow",
                    "free_cashflow", "total_cash", "total_debt"):
            self.assertIsNone(out[key], key)

    def test_fcf_subtracts_capex_regardless_of_presented_sign(self):
        # Live AAPL 10-K presents capex as a negative outflow row; FCF must
        # be OCF - |capex|, never OCF + capex.
        fix = {"success": True, "data": {"cash_flow": {"data": _statement(
            ["us-gaap_NetCashProvidedByUsedInOperatingActivities",
             "us-gaap_PaymentsToAcquirePropertyPlantAndEquipment"],
            [110.0, -13.0])}}}
        out = relationship_map.extract_financials_from_statements(fix)
        self.assertEqual(out["free_cashflow"], 97.0)

    def test_ebitda_needs_both_components(self):
        # Operating income present but no D&A anywhere -> EBITDA is missing,
        # never fabricated from a partial sum.
        fix = {"success": True, "data": {"income_statement": {"data": _statement(
            ["us-gaap_OperatingIncomeLoss"], [133.0])}}}
        out = relationship_map.extract_financials_from_statements(fix)
        self.assertIsNone(out["ebitda"])


# ── 2. Insider ring from Form 4 rows ────────────────────────────────────────

class TestInsiderRing(unittest.TestCase):
    def test_dedupe_and_direction(self):
        insiders = relationship_map.build_insider_holders(INSIDER_ROWS)
        names = [i["name"] for i in insiders]
        self.assertEqual(names, ["Tim Cook", "Luca Maestri"])  # case-insensitive dedupe, no None
        self.assertEqual(insiders[0]["last_transaction"], "sell")  # newest row wins
        self.assertEqual(insiders[0]["title"], "CEO")
        self.assertEqual(insiders[1]["last_transaction"], "buy")   # code M with no type text
        # Form 4 rows carry transaction shares, not holdings — never surfaced
        # as holdings.
        self.assertEqual(insiders[0]["shares"], 0.0)

    def test_limit(self):
        rows = [{"filing_date": "2026-01-01", "insider": f"P{i}",
                 "position": "", "transaction_type": "Sale", "code": "S"}
                for i in range(20)]
        self.assertEqual(len(relationship_map.build_insider_holders(rows, limit=5)), 5)


# ── 3. SC 13D/G major holders ───────────────────────────────────────────────

class Test13DGMajorHolders(unittest.TestCase):
    def setUp(self):
        self._orig_company = forms_13dg.Company
        self._orig_check = forms_13dg.check_edgar_available
        forms_13dg.Company = FakeCompany
        forms_13dg.check_edgar_available = lambda: None

    def tearDown(self):
        forms_13dg.Company = self._orig_company
        forms_13dg.check_edgar_available = self._orig_check
        FakeCompany.filings = []

    def test_newest_filing_per_holder_wins(self):
        FakeCompany.filings = [
            FakeFiling("SCHEDULE 13G", "2026-04-29",
                       FakeSchedule([FakePerson("The Vanguard Group", 7.48, 1.1e9)])),
            FakeFiling("SC 13G/A", "2024-02-13",
                       FakeSchedule([FakePerson("VANGUARD GROUP", 9.0, 1.4e9)])),
            FakeFiling("SC 13G/A", "2024-02-14",
                       FakeSchedule([FakePerson("Berkshire Hathaway Inc", None, None)], structured=False)),
        ]
        # "The Vanguard Group" vs "VANGUARD GROUP" differ, so both survive —
        # dedupe is exact-name (case/whitespace-insensitive), not fuzzy.
        result = forms_13dg.get_major_holders("AAPL")
        self.assertTrue(result["success"])
        names = [h["name"] for h in result["data"]]
        self.assertEqual(names, ["The Vanguard Group", "VANGUARD GROUP", "Berkshire Hathaway Inc"])
        self.assertEqual(result["data"][0]["percent_of_class"], 7.48)
        self.assertEqual(result["data"][0]["form"], "SCHEDULE 13G")
        self.assertEqual(result["data"][0]["filing_date"], "2026-04-29")
        self.assertFalse(result["data"][0]["is_amendment"])
        self.assertTrue(result["data"][1]["is_amendment"])

    def test_exact_duplicate_holder_keeps_newest(self):
        FakeCompany.filings = [
            FakeFiling("SCHEDULE 13G/A", "2026-03-26",
                       FakeSchedule([FakePerson("The Vanguard Group", 0.0, 0.0)])),
            FakeFiling("SCHEDULE 13G", "2025-07-29",
                       FakeSchedule([FakePerson("the  vanguard group", 9.47, 1.4e9)])),
        ]
        result = forms_13dg.get_major_holders("AAPL")
        self.assertEqual(result["count"], 1)
        # The newest structured amendment reports 0.0 — a real exit, kept
        # verbatim (not replaced by the older 9.47).
        self.assertEqual(result["data"][0]["percent_of_class"], 0.0)
        self.assertEqual(result["data"][0]["filing_date"], "2026-03-26")

    def test_unstructured_filing_has_no_fabricated_numbers(self):
        FakeCompany.filings = [
            FakeFiling("SC 13G/A", "2024-02-12",
                       FakeSchedule([FakePerson("BlackRock Inc.", 0.0, 5.0e8)], structured=False)),
        ]
        result = forms_13dg.get_major_holders("AAPL")
        holder = result["data"][0]
        self.assertIsNone(holder["percent_of_class"])
        self.assertIsNone(holder["shares"])
        self.assertEqual(holder["form"], "SC 13G/A")     # provenance survives
        self.assertEqual(holder["filing_date"], "2024-02-12")

    def test_filings_but_zero_persons_is_parser_error(self):
        FakeCompany.filings = [
            FakeFiling("SC 13G", "2024-02-12", FakeSchedule([])),
            FakeFiling("SC 13D", "2023-05-01", Exception("boom")),
        ]
        result = forms_13dg.get_major_holders("AAPL")
        self.assertIn("error", result)
        self.assertNotIn("success", result)

    def test_no_filings_is_empty_success(self):
        FakeCompany.filings = []
        result = forms_13dg.get_major_holders("AAPL")
        self.assertTrue(result["success"])
        self.assertEqual(result["data"], [])

    def test_aggregate_respects_limit(self):
        rows = [{"name": f"Holder {i}"} for i in range(30)]
        self.assertEqual(len(forms_13dg.aggregate_reporting_persons(rows, 10)), 10)


# ── 4. Rewired sections never touch yfinance ────────────────────────────────

class TestNoYfinanceInRewiredSections(unittest.TestCase):
    def test_edgar_sections_run_with_yfinance_blocked(self):
        # sys.modules["yfinance"] is None (module top) — any import attempt
        # raises. Stub the three EDGAR fetchers and run the full rewired path.
        fin_mod = types.SimpleNamespace(get_financials=lambda t: FIN_FIXTURE)
        ins_mod = types.SimpleNamespace(get_insider_transactions=lambda t, limit: {
            "success": True, "data": INSIDER_ROWS})
        dg_mod = types.SimpleNamespace(get_major_holders=lambda t, limit, max_scan: {
            "success": True,
            "data": [{"name": "The Vanguard Group", "percent_of_class": 7.48,
                      "shares": 1.1e9, "person_type": "IA",
                      "form": "SCHEDULE 13G", "filing_date": "2026-04-29",
                      "is_amendment": False}]})
        base_mod = types.SimpleNamespace(initialize_edgar=lambda: None)

        # `from mcp.edgar import X` resolves through the package's attributes
        # once submodules are loaded, so stub the attributes (and sys.modules
        # for the never-imported ones, which the import machinery consults).
        import mcp.edgar as edgar_pkg
        stubs = {"base": base_mod, "financials": fin_mod,
                 "forms_insider": ins_mod, "forms_13dg": dg_mod}
        saved_attrs = {k: getattr(edgar_pkg, k, None) for k in stubs}
        saved_mods = {f"mcp.edgar.{k}": sys.modules.get(f"mcp.edgar.{k}") for k in stubs}
        for k, mod in stubs.items():
            setattr(edgar_pkg, k, mod)
            sys.modules[f"mcp.edgar.{k}"] = mod
        try:
            out = relationship_map.fetch_edgar_sections("AAPL")
        finally:
            for k in stubs:
                if saved_attrs[k] is None:
                    delattr(edgar_pkg, k)
                else:
                    setattr(edgar_pkg, k, saved_attrs[k])
                name = f"mcp.edgar.{k}"
                if saved_mods[name] is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved_mods[name]

        self.assertEqual(out["section_errors"], {})
        self.assertEqual(out["financials"]["revenue"], 416.0)
        self.assertEqual(out["insider_holders"][0]["name"], "Tim Cook")
        holder = out["institutional_holders"][0]
        self.assertEqual(holder["name"], "The Vanguard Group")
        self.assertEqual(holder["percentage"], 7.48)
        self.assertEqual(holder["source_form"], "SCHEDULE 13G")
        self.assertEqual(holder["type"], "institutional")

    def test_peer_candidates_need_no_network(self):
        peers = relationship_map.peer_candidates("AAPL", "Consumer Electronics", "Technology")
        self.assertNotIn("AAPL", peers)
        self.assertIn("MSFT", peers)


if __name__ == "__main__":
    unittest.main(verbosity=2)
