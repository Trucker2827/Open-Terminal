"""
Issue #101 — EDGAR XBRL → FinancialsData adapter (scripts/edgar_financials.py).

Covers the pure adapter that turns an mcp.edgar.financials.get_financials()
result (statement dataframe to_dict(): concept/label/dimension/abstract
columns + 'YYYY-MM-DD (FY)' period columns) into the FinancialsData shape
the Qt service parses (statement → period → line_item → value):

1. Canonical us-gaap concepts map to the yfinance-style line-item names the
   Financials tab looks up ("Total Revenue", "Operating Cash Flow", …).
2. Dimensioned (segment) and abstract (header) rows are skipped — the
   consolidated row wins, never a product/geography breakdown.
3. Missing/NaN values stay missing — no zero-filled rows, no fabrication.
4. Values pass through with their reported sign — no sign rewriting (the
   tab normalises capex/dividend signs itself, for either convention).
5. Unmapped concepts keep their XBRL label so the raw table stays complete.
6. Errors pass through as {"error": ...}; an all-empty result is an error
   (that is what triggers the service's yfinance fallback).
7. The CLI refuses suffixed (non-US) symbols and bad usage with error JSON.

Stdlib-only (unittest + fixtures) — runs without edgartools/yfinance/network.
"""

import json
import os
import sys
import unittest

SCRIPTS_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, SCRIPTS_DIR)

# The adapter must work with edgartools and yfinance entirely absent.
# Installing None makes any `import edgar`/`import yfinance` raise
# ImportError immediately, so a network fetch can never sneak into a test.
sys.modules["edgar"] = None
sys.modules["yfinance"] = None

import edgar_financials  # noqa: E402


# ── Fixtures ────────────────────────────────────────────────────────────────

FY25 = "2025-09-27 (FY)"
FY24 = "2024-09-28 (FY)"


def stmt(rows, periods):
    """
    Build a statement to_dict() like edgartools' to_dataframe().to_dict().

    rows: list of (concept, label, dimension, abstract)
    periods: {column_name: [value per row]}
    """
    d = {
        "concept": {i: r[0] for i, r in enumerate(rows)},
        "label": {i: r[1] for i, r in enumerate(rows)},
        "dimension": {i: r[2] for i, r in enumerate(rows)},
        "abstract": {i: r[3] for i, r in enumerate(rows)},
    }
    for col, vals in periods.items():
        d[col] = {i: v for i, v in enumerate(vals) if v is not None}
    return d


INCOME_ROWS = [
    ("us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
     "Total net sales", None, False),
    ("us-gaap_GrossProfit", "Gross margin", None, False),
    ("us-gaap_OperatingIncomeLoss", "Operating income", None, False),
    ("us-gaap_NetIncomeLoss", "Net income", None, False),
    ("us-gaap_ResearchAndDevelopmentExpense",
     "Research and development", None, False),
]

INCOME_FIXTURE = stmt(
    INCOME_ROWS,
    {FY25: [416.0, 195.0, 133.0, 112.0, 34.0],
     FY24: [391.0, 180.0, 123.0, 93.0, 31.0]})


def full_fin_result(income=None, balance=None, cashflow=None):
    """Wrap statement dicts in the get_financials() result envelope."""
    def sec(data):
        return {"type": "Statement", "data": data if data is not None else {}}
    return {
        "success": True,
        "data": {
            "ticker": "AAPL",
            "income_statement": sec(income),
            "balance_sheet": sec(balance),
            "cash_flow": sec(cashflow),
        },
    }


# ── adapt_statement ─────────────────────────────────────────────────────────

class AdaptStatementTest(unittest.TestCase):

    def test_canonical_concepts_map_to_yfinance_names(self):
        out = edgar_financials.adapt_statement(
            INCOME_FIXTURE, edgar_financials.CANONICAL_INCOME)
        self.assertEqual(sorted(out), ["2024-09-28", "2025-09-27"])
        latest = out["2025-09-27"]
        self.assertEqual(latest["Total Revenue"], 416.0)
        self.assertEqual(latest["Gross Profit"], 195.0)
        self.assertEqual(latest["Operating Income"], 133.0)
        self.assertEqual(latest["Net Income"], 112.0)
        self.assertEqual(out["2024-09-28"]["Total Revenue"], 391.0)

    def test_unmapped_concepts_keep_their_xbrl_label(self):
        out = edgar_financials.adapt_statement(
            INCOME_FIXTURE, edgar_financials.CANONICAL_INCOME)
        self.assertEqual(out["2025-09-27"]["Research and development"], 34.0)

    def test_dimensioned_rows_skipped_consolidated_wins(self):
        # Segment breakdown listed BEFORE the consolidated row: skipping must
        # come from the dimension flag, not from row-order luck.
        rows = [
            ("us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
             "Products", "srt_ProductOrServiceAxis", False),
            ("us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
             "Services", "srt_ProductOrServiceAxis", False),
            ("us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
             "Total net sales", None, False),
        ]
        out = edgar_financials.adapt_statement(
            stmt(rows, {FY25: [307.0, 109.0, 416.0]}),
            edgar_financials.CANONICAL_INCOME)
        self.assertEqual(out["2025-09-27"]["Total Revenue"], 416.0)
        self.assertNotIn("Products", out["2025-09-27"])

    def test_abstract_rows_skipped(self):
        rows = [
            ("us-gaap_OperatingExpensesAbstract",
             "Operating expenses:", None, True),
            ("us-gaap_OperatingIncomeLoss", "Operating income", None, False),
        ]
        out = edgar_financials.adapt_statement(
            stmt(rows, {FY25: [0.0, 133.0]}),
            edgar_financials.CANONICAL_INCOME)
        self.assertEqual(out["2025-09-27"],
                         {"Operating Income": 133.0})

    def test_missing_and_nan_values_stay_missing(self):
        rows = [
            ("us-gaap_GrossProfit", "Gross margin", None, False),
            ("us-gaap_NetIncomeLoss", "Net income", None, False),
            ("us-gaap_OperatingIncomeLoss", "Operating income", None, False),
        ]
        out = edgar_financials.adapt_statement(
            stmt(rows, {FY25: [None, float("nan"), 133.0]}),
            edgar_financials.CANONICAL_INCOME)
        self.assertEqual(out["2025-09-27"], {"Operating Income": 133.0})
        self.assertNotIn("Gross Profit", out["2025-09-27"])
        self.assertNotIn("Net Income", out["2025-09-27"])

    def test_values_pass_through_with_reported_sign(self):
        # XBRL cash-flow payments appear with either sign depending on the
        # concept weight; the adapter must not rewrite them. The tab
        # normalises signs itself.
        rows = [
            ("us-gaap_PaymentsToAcquirePropertyPlantAndEquipment",
             "Payments for acquisition of PP&E", None, False),
            ("us-gaap_PaymentsOfDividends",
             "Payments for dividends", None, False),
        ]
        out = edgar_financials.adapt_statement(
            stmt(rows, {FY25: [-11.0, 15.0]}),
            edgar_financials.CANONICAL_CASHFLOW)
        self.assertEqual(out["2025-09-27"]["Capital Expenditure"], -11.0)
        self.assertEqual(out["2025-09-27"]["Cash Dividends Paid"], 15.0)

    def test_plain_date_period_column_accepted(self):
        out = edgar_financials.adapt_statement(
            stmt([("us-gaap_Assets", "Total assets", None, False)],
                 {"2025-09-27": [364.0]}),
            edgar_financials.CANONICAL_BALANCE)
        self.assertEqual(out, {"2025-09-27": {"Total Assets": 364.0}})

    def test_non_period_and_malformed_input(self):
        self.assertEqual(edgar_financials.adapt_statement(None, {}), {})
        self.assertEqual(edgar_financials.adapt_statement("Balance Sheet\n...", {}), {})
        # Only metadata columns, no period columns → empty.
        out = edgar_financials.adapt_statement(
            stmt([("us-gaap_Assets", "Total assets", None, False)], {}), {})
        self.assertEqual(out, {})


# ── edgar_to_financials_data ────────────────────────────────────────────────

class ToFinancialsDataTest(unittest.TestCase):

    def test_full_payload_shape_and_source(self):
        bal = stmt([("us-gaap_Assets", "Total assets", None, False)],
                   {FY25: [364.0]})
        cf = stmt([("us-gaap_NetCashProvidedByUsedInOperatingActivities",
                    "Cash generated by operating activities", None, False)],
                  {FY25: [110.0]})
        out = edgar_financials.edgar_to_financials_data(
            "AAPL", full_fin_result(INCOME_FIXTURE, bal, cf))
        self.assertEqual(out["symbol"], "AAPL")
        self.assertEqual(out["source"], "edgar")
        self.assertEqual(
            out["income_statement"]["2025-09-27"]["Total Revenue"], 416.0)
        self.assertEqual(
            out["balance_sheet"]["2025-09-27"]["Total Assets"], 364.0)
        self.assertEqual(
            out["cash_flow"]["2025-09-27"]["Operating Cash Flow"], 110.0)
        json.dumps(out)  # must be JSON-serializable end to end

    def test_error_result_passes_through(self):
        err = {"error": {"command": "get_financials",
                         "error": "No company found for AAPL",
                         "type": "EdgarError"}}
        out = edgar_financials.edgar_to_financials_data("AAPL", err)
        self.assertIn("No company found", out["error"])
        self.assertEqual(out["symbol"], "AAPL")
        self.assertNotIn("income_statement", out)

    def test_all_empty_statements_is_an_error_not_a_silent_success(self):
        out = edgar_financials.edgar_to_financials_data(
            "AAPL", full_fin_result({}, {}, {}))
        self.assertIn("error", out)
        self.assertNotIn("income_statement", out)

    def test_string_statement_data_is_empty_not_a_crash(self):
        # get_financials falls back to str(statement) when to_dataframe is
        # unavailable — that must read as missing, never as parsed numbers.
        out = edgar_financials.edgar_to_financials_data(
            "AAPL", full_fin_result("Balance Sheet\n...", "x", "y"))
        self.assertIn("error", out)


# ── CLI entry point ─────────────────────────────────────────────────────────

class MainTest(unittest.TestCase):

    def test_suffixed_symbol_refused_without_edgar(self):
        out = json.loads(edgar_financials.main(["financials", "RELIANCE.NS"]))
        self.assertIn("error", out)
        self.assertIn("non-US", out["error"])
        self.assertEqual(out["symbol"], "RELIANCE.NS")

    def test_usage_errors(self):
        self.assertIn("Usage", json.loads(edgar_financials.main([]))["error"])
        self.assertIn("Usage", json.loads(
            edgar_financials.main(["financials"]))["error"])
        self.assertIn("Usage", json.loads(
            edgar_financials.main(["quote", "AAPL"]))["error"])

    def test_us_symbol_with_edgar_unavailable_is_error_json(self):
        # edgar is blocked in this test process; the fetch path must still
        # return structured error JSON (the service falls back on it).
        out = json.loads(edgar_financials.main(["financials", "AAPL"]))
        self.assertIn("error", out)


if __name__ == "__main__":
    unittest.main()
