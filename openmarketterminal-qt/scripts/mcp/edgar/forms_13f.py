"""
13F-HR Institutional Holdings Extraction
=========================================

Extract institutional holdings data from 13F-HR filings including:
- Portfolio holdings (ticker, shares, value)
- Fund manager information
- Quarter-over-quarter changes
- Top positions
- Total portfolio value
"""

from typing import Dict, Any, Optional
import traceback

try:
    from edgar import Company
    EDGAR_AVAILABLE = True
except ImportError:
    EDGAR_AVAILABLE = False

from .base import EdgarError, check_edgar_available


def _num(x):
    """Coerce numpy/None/str to a plain float, or None (drops NaN)."""
    if x is None:
        return None
    try:
        f = float(x)
        return f if f == f else None
    except (TypeError, ValueError):
        return None


def get_13f_holdings(ticker: str, quarters: int = 2) -> Dict[str, Any]:
    """
    Get institutional holdings from 13F-HR filings

    Args:
        ticker: Stock ticker symbol
        quarters: Number of quarters to retrieve

    Returns:
        Holdings data for specified quarters
    """
    try:
        check_edgar_available()
        company = Company(ticker)
        filings = company.get_filings(form="13F-HR")

        if not filings or len(filings) == 0:
            return {
                "success": True,
                "data": [],
                "count": 0
            }

        holdings_data = []
        count = 0

        for filing in filings:
            if count >= quarters:
                break

            try:
                thirteenf = filing.obj()

                if not thirteenf:
                    continue

                # Get holdings
                holdings_list = []
                if hasattr(thirteenf, 'holdings'):
                    holdings = thirteenf.holdings

                    # Convert to list if needed
                    if hasattr(holdings, 'to_pandas'):
                        df = holdings.to_pandas()
                        holdings_list = df.to_dict('records')
                    elif hasattr(holdings, '__iter__'):
                        holdings_list = list(holdings)

                holdings_data.append({
                    "filing_date": str(filing.filing_date),
                    "report_period": str(thirteenf.report_period) if hasattr(thirteenf, 'report_period') else None,
                    "manager_name": thirteenf.manager_name if hasattr(thirteenf, 'manager_name') else None,
                    "total_value": float(thirteenf.total_value) if hasattr(thirteenf, 'total_value') and thirteenf.total_value else None,
                    "holdings_count": len(holdings_list),
                    "holdings": holdings_list
                })

                count += 1

            except Exception as e:
                continue

        return {
            "success": True,
            "data": holdings_data,
            "count": len(holdings_data)
        }
    except Exception as e:
        return {"error": EdgarError("get_13f_holdings", str(e), traceback.format_exc()).to_dict()}


def get_13f_top_holdings(ticker: str, top_n: int = 20) -> Dict[str, Any]:
    """
    Get top holdings from latest 13F filing

    Args:
        ticker: Stock ticker symbol (of the fund/institution)
        top_n: Number of top holdings to return

    Returns:
        Top N holdings by value
    """
    try:
        check_edgar_available()
        company = Company(ticker)
        filing = company.get_filings(form="13F-HR").latest(1)

        if not filing:
            return {"error": EdgarError("get_13f_top_holdings",
                                        f"No 13F-HR filing found for {ticker}").to_dict()}

        thirteenf = filing.obj()
        # The real holdings live in the parsed information table (a DataFrame with
        # Issuer/Cusip/Value/SharesPrnAmount/Ticker columns). The old code iterated
        # `holdings` and got the column NAMES ("Issuer"/"Class"/"Cusip") as rows.
        df = getattr(thirteenf, "infotable", None)
        if df is None or len(df) == 0:
            return {"error": EdgarError(
                "get_13f_top_holdings",
                f"13F filing for {ticker} has no parsed holdings — the 13F parser may be out of date").to_dict()}

        cols = {c.lower(): c for c in df.columns}
        vcol, icol = cols.get("value"), cols.get("issuer")
        scol, ccol = cols.get("sharesprnamount"), cols.get("cusip")
        tcol, clcol = cols.get("ticker"), cols.get("class")

        ordered = df.sort_values(by=vcol, ascending=False).head(top_n) if vcol else df.head(top_n)
        holdings = []
        for _, r in ordered.iterrows():
            issuer = str(r[icol]).strip() if icol and r[icol] is not None else None
            holdings.append({
                "issuer": issuer,
                "ticker": (str(r[tcol]).strip() or None) if tcol and r[tcol] is not None else None,
                "cusip": str(r[ccol]).strip() if ccol and r[ccol] is not None else None,
                "class": str(r[clcol]).strip() if clcol and r[clcol] is not None else None,
                "value": _num(r[vcol]) if vcol else None,
                "shares": _num(r[scol]) if scol else None,
            })

        # Header-row guard (review condition #3): the exact bug we're fixing was
        # "holdings" being the literal header tokens. Reject that as a parse failure.
        HEADERS = {"issuer", "class", "cusip", "value"}
        if not holdings or all((h["issuer"] or "").strip().lower() in HEADERS for h in holdings):
            return {"error": EdgarError(
                "get_13f_top_holdings",
                f"13F holdings for {ticker} did not parse into issuer rows").to_dict()}

        total_value = _num(df[vcol].sum()) if vcol else None
        return {
            "success": True,
            "data": {
                "filing_date": str(filing.filing_date),
                "report_period": str(getattr(thirteenf, "report_period", "")) or None,
                "manager_name": (getattr(thirteenf, "management_company_name", None)
                                 or getattr(thirteenf, "manager_name", None)),
                "total_value": total_value,
                "holdings_count": int(len(df)),
                "top_holdings": holdings,
                "count": len(holdings),
            }
        }
    except Exception as e:
        return {"error": EdgarError("get_13f_top_holdings", str(e), traceback.format_exc()).to_dict()}


def get_13f_manager_info(ticker: str) -> Dict[str, Any]:
    """
    Get fund manager information from latest 13F

    Args:
        ticker: Stock ticker symbol (of the fund/institution)

    Returns:
        Manager information and portfolio summary
    """
    try:
        check_edgar_available()
        company = Company(ticker)
        filing = company.get_filings(form="13F-HR").latest(1)

        if not filing:
            return {
                "error": EdgarError("get_13f_manager_info",
                                  f"No 13F-HR filing found for {ticker}").to_dict()
            }

        thirteenf = filing.obj()

        if not thirteenf:
            return {
                "error": EdgarError("get_13f_manager_info",
                                  "Could not parse 13F filing").to_dict()
            }

        # Extract manager info
        manager_info = {
            "manager_name": thirteenf.manager_name if hasattr(thirteenf, 'manager_name') else None,
            "report_period": str(thirteenf.report_period) if hasattr(thirteenf, 'report_period') else None,
            "filing_date": str(filing.filing_date),
            "total_value": float(thirteenf.total_value) if hasattr(thirteenf, 'total_value') and thirteenf.total_value else None,
            "total_holdings": thirteenf.total_holdings if hasattr(thirteenf, 'total_holdings') else None,
            "management_company_name": thirteenf.management_company_name if hasattr(thirteenf, 'management_company_name') else None,
        }

        # Get portfolio managers if available
        if hasattr(thirteenf, 'get_portfolio_managers'):
            try:
                managers = thirteenf.get_portfolio_managers()
                manager_info["portfolio_managers"] = managers
            except:
                pass

        return {
            "success": True,
            "data": manager_info
        }
    except Exception as e:
        return {"error": EdgarError("get_13f_manager_info", str(e), traceback.format_exc()).to_dict()}


def get_13f_summary(ticker: str) -> Dict[str, Any]:
    """
    Get summary statistics from latest 13F filing

    Args:
        ticker: Stock ticker symbol (of the fund/institution)

    Returns:
        Portfolio summary statistics
    """
    try:
        check_edgar_available()
        company = Company(ticker)
        filing = company.get_filings(form="13F-HR").latest(1)

        if not filing:
            return {
                "error": EdgarError("get_13f_summary",
                                  f"No 13F-HR filing found for {ticker}").to_dict()
            }

        thirteenf = filing.obj()

        if not thirteenf:
            return {
                "error": EdgarError("get_13f_summary",
                                  "Could not parse 13F filing").to_dict()
            }

        summary = {
            "manager_name": thirteenf.manager_name if hasattr(thirteenf, 'manager_name') else None,
            "report_period": str(thirteenf.report_period) if hasattr(thirteenf, 'report_period') else None,
            "filing_date": str(filing.filing_date),
            "total_value": float(thirteenf.total_value) if hasattr(thirteenf, 'total_value') and thirteenf.total_value else None,
            "total_holdings": thirteenf.total_holdings if hasattr(thirteenf, 'total_holdings') else 0,
        }

        # Calculate additional stats from holdings
        if hasattr(thirteenf, 'holdings'):
            holdings = thirteenf.holdings
            if hasattr(holdings, 'to_pandas'):
                df = holdings.to_pandas()

                if 'Value' in df.columns or 'value' in df.columns:
                    value_col = 'Value' if 'Value' in df.columns else 'value'

                    # Top 10 concentration
                    top_10_value = df.nlargest(10, value_col)[value_col].sum()
                    total_value = df[value_col].sum()

                    summary["top_10_concentration"] = float(top_10_value / total_value * 100) if total_value > 0 else 0
                    summary["average_position_size"] = float(total_value / len(df)) if len(df) > 0 else 0

        return {
            "success": True,
            "data": summary
        }
    except Exception as e:
        return {"error": EdgarError("get_13f_summary", str(e), traceback.format_exc()).to_dict()}
