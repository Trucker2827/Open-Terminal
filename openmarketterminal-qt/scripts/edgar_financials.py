"""
EDGAR XBRL financial statements in FinancialsData shape (issue #101).

CLI entry point the Qt EquityResearchService calls the same way it calls
yfinance_data.py:

    python edgar_financials.py financials AAPL

Prints JSON shaped exactly like yfinance_data.py's `financials` output —
statement → period → line_item → value — plus `"source": "edgar"`, so
EquityResearchService::parse_financials consumes either source unchanged.

Data comes from the repo's existing EDGAR XBRL pipeline
(scripts/mcp/edgar/financials.py, edgartools under the hood). The adapter
in this module is pure and unit-tested offline
(tests/test_edgar_financials_adapter.py).

Honesty rules:
- Only consolidated, non-abstract rows are emitted; segment breakdowns
  (dimension flag) and header rows (abstract flag) are skipped.
- Missing/NaN line items stay missing — no zero-filled rows.
- Values keep their reported sign; the UI normalises cash-flow payment
  signs itself for either convention.
- Any failure (CIK lookup, edgartools absent, empty statements, non-US
  symbol) returns {"error": ...} so the caller falls back to yfinance.
"""

import json
import re
import sys
import time

# ── us-gaap concept → yfinance-style line-item name ─────────────────────────
# Only true 1:1 mappings, so the Financials tab's metric cards find the
# names they already look up. Anything unmapped keeps its XBRL label and
# still appears in the raw statement table. Aggregates yfinance invents
# (Total Debt, EBITDA) have no single XBRL concept and are NOT derived here.

CANONICAL_INCOME = {
    "us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax": "Total Revenue",
    "us-gaap_Revenues": "Total Revenue",
    "us-gaap_SalesRevenueNet": "Total Revenue",
    "us-gaap_GrossProfit": "Gross Profit",
    "us-gaap_OperatingIncomeLoss": "Operating Income",
    "us-gaap_NetIncomeLoss": "Net Income",
    "us-gaap_InterestExpense": "Interest Expense",
    "us-gaap_InterestExpenseNonoperating": "Interest Expense Non Operating",
}

CANONICAL_BALANCE = {
    "us-gaap_Assets": "Total Assets",
    "us-gaap_Liabilities": "Total Liabilities",
    "us-gaap_StockholdersEquity": "Stockholders Equity",
    "us-gaap_StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest":
        "Total Equity",
    "us-gaap_CashAndCashEquivalentsAtCarryingValue": "Cash And Cash Equivalents",
    "us-gaap_AssetsCurrent": "Current Assets",
    "us-gaap_LiabilitiesCurrent": "Current Liabilities",
    "us-gaap_InventoryNet": "Inventory",
    "us-gaap_LongTermDebtNoncurrent": "Long Term Debt",
}

CANONICAL_CASHFLOW = {
    "us-gaap_NetCashProvidedByUsedInOperatingActivities": "Operating Cash Flow",
    "us-gaap_NetCashProvidedByUsedInOperatingActivitiesContinuingOperations":
        "Operating Cash Flow",
    "us-gaap_NetCashProvidedByUsedInInvestingActivities": "Investing Cash Flow",
    "us-gaap_NetCashProvidedByUsedInInvestingActivitiesContinuingOperations":
        "Investing Cash Flow",
    "us-gaap_NetCashProvidedByUsedInFinancingActivities": "Financing Cash Flow",
    "us-gaap_NetCashProvidedByUsedInFinancingActivitiesContinuingOperations":
        "Financing Cash Flow",
    "us-gaap_PaymentsToAcquirePropertyPlantAndEquipment": "Capital Expenditure",
    "us-gaap_PaymentsOfDividends": "Cash Dividends Paid",
    "us-gaap_PaymentsOfDividendsCommonStock": "Cash Dividends Paid",
    "us-gaap_PaymentsForRepurchaseOfCommonStock": "Repurchase Of Capital Stock",
    "us-gaap_DepreciationDepletionAndAmortization": "Depreciation And Amortization",
    "us-gaap_DepreciationAmortizationAndAccretionNet": "Depreciation And Amortization",
}

# 'YYYY-MM-DD (FY)' / 'YYYY-MM-DD' statement columns; everything else in a
# to_dict() (concept/label/dimension/abstract/...) is row metadata.
_PERIOD_COL = re.compile(r"^(\d{4}-\d{2}-\d{2})")


def adapt_statement(df_dict, canonical):
    """
    One XBRL statement to_dict() → {period: {line_item: value}}.

    df_dict is edgartools' statement.to_dataframe().to_dict(): metadata
    columns (concept/label/dimension/abstract) plus one column per period.
    Consolidated (non-dimension, non-abstract) rows only; the line-item name
    is the canonical yfinance-style name when the concept has one, else the
    XBRL label. First row wins on a name collision (the statement's primary
    presentation of that concept). Missing/NaN values are omitted.
    """
    if not isinstance(df_dict, dict):
        return {}
    concepts = df_dict.get("concept")
    if not isinstance(concepts, dict):
        return {}
    labels = df_dict.get("label") or {}
    dimensions = df_dict.get("dimension") or {}
    abstracts = df_dict.get("abstract") or {}

    out = {}
    for col in df_dict:
        m = _PERIOD_COL.match(str(col))
        if not m:
            continue
        period = m.group(1)
        values = df_dict.get(col) or {}
        row = out.setdefault(period, {})
        for idx, concept in concepts.items():
            if dimensions.get(idx) or abstracts.get(idx):
                continue
            try:
                val = float(values.get(idx))
            except (TypeError, ValueError):
                continue
            if val != val:  # NaN
                continue
            name = canonical.get(concept) or str(labels.get(idx) or concept)
            if name not in row:
                row[name] = val
    return {p: r for p, r in out.items() if r}


def edgar_to_financials_data(symbol, fin_result):
    """
    mcp.edgar.financials.get_financials() result → FinancialsData-shaped
    dict, or {"error": ..., "symbol": ...} when EDGAR cannot serve it.
    """
    if not isinstance(fin_result, dict) or fin_result.get("error"):
        err = fin_result.get("error") if isinstance(fin_result, dict) else None
        if isinstance(err, dict):
            msg = err.get("error") or "EDGAR error"
        else:
            msg = str(err) if err else "EDGAR returned no result"
        return {"error": msg, "symbol": symbol}

    data = fin_result.get("data") or {}

    def section(key, canonical):
        return adapt_statement((data.get(key) or {}).get("data"), canonical)

    income = section("income_statement", CANONICAL_INCOME)
    balance = section("balance_sheet", CANONICAL_BALANCE)
    cashflow = section("cash_flow", CANONICAL_CASHFLOW)

    if not (income or balance or cashflow):
        return {"error": f"EDGAR returned no statement data for {symbol}",
                "symbol": symbol}

    return {
        "symbol": symbol,
        "source": "edgar",
        "income_statement": income,
        "balance_sheet": balance,
        "cash_flow": cashflow,
        "timestamp": int(time.time()),
    }


def fetch_financials(symbol):
    """Live EDGAR fetch (network). Import is lazy so the adapter stays
    importable and testable without edgartools installed."""
    try:
        from mcp.edgar.base import initialize_edgar
        from mcp.edgar.financials import get_financials
        initialize_edgar()
        return edgar_to_financials_data(symbol, get_financials(symbol))
    except Exception as e:
        return {"error": str(e), "symbol": symbol}


def main(args=None):
    args = list(sys.argv[1:] if args is None else args)
    if len(args) < 2 or args[0] != "financials":
        return json.dumps(
            {"error": "Usage: python edgar_financials.py financials <symbol>"})
    symbol = args[1].strip()
    if "." in symbol:
        # Suffixed symbols (.NS/.BO/...) are non-US listings; EDGAR serves
        # US filers only. The service goes straight to yfinance for these —
        # this guard keeps a direct CLI call honest too.
        return json.dumps(
            {"error": f"non-US listing {symbol}: EDGAR serves US filers only",
             "symbol": symbol})
    return json.dumps(fetch_financials(symbol.upper()))


if __name__ == "__main__":
    print(main())
