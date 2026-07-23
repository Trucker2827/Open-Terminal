#!/usr/bin/env python3
"""
Relationship Map Data Fetcher — v3
Fetches comprehensive corporate intelligence for the relationship map.

Sources (issue #83 rewire):
- SEC EDGAR (mcp.edgar, via edgartools):
  * financial statement values — XBRL us-gaap concept mapping over
    financials.get_financials()
  * insider ring — Form 4 rows from forms_insider.get_insider_transactions()
  * institutional ring — >5% beneficial owners from SC 13D/G filings via
    forms_13dg.get_major_holders(), with filing provenance
- Yahoo Finance (yfinance) — the sections with no EDGAR/native replacement
  yet: company profile/ratios, governance risk, technicals, short interest,
  analyst data, officers, earnings calendar, mutual-fund holders.
- Peer *metrics* are NOT fetched here anymore: this script only emits
  "peer_tickers" (curated candidates); the terminal's native
  EquityResearchService fetches the metrics (RelationshipMapService).

Honesty contract: a section whose source fails reads as missing (empty
list / zero fields) and the failure is reported in "section_errors" —
never silently backfilled from another source.

Usage: python relationship_map.py <TICKER>
Output: JSON to stdout
"""

import json
import re
import sys


def _safe_float(val, default=0.0):
    try:
        return float(val) if val is not None else default
    except (TypeError, ValueError):
        return default


def _safe_int(val, default=0):
    try:
        return int(val) if val is not None else default
    except (TypeError, ValueError):
        return default


def _safe_str(val, default=""):
    return str(val) if val is not None else default


# ═══════════════════════════════════════════════════════════════════════════
# EDGAR SECTIONS (no yfinance anywhere below this banner — tested)
# ═══════════════════════════════════════════════════════════════════════════

# us-gaap concept aliases, in priority order, for the flat financial fields
# the map displays. Values come from the latest period column of the XBRL
# statements returned by mcp.edgar.financials.get_financials().
CONCEPT_ALIASES = {
    "revenue": [
        "us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax",
        "us-gaap_Revenues",
        "us-gaap_SalesRevenueNet",
    ],
    "operating_income": ["us-gaap_OperatingIncomeLoss"],
    "depreciation_amortization": [
        "us-gaap_DepreciationDepletionAndAmortization",
        "us-gaap_DepreciationAndAmortization",
        "us-gaap_DepreciationAmortizationAndAccretionNet",
    ],
    "operating_cashflow": [
        "us-gaap_NetCashProvidedByUsedInOperatingActivities",
        "us-gaap_NetCashProvidedByUsedInOperatingActivitiesContinuingOperations",
    ],
    "capital_expenditures": ["us-gaap_PaymentsToAcquirePropertyPlantAndEquipment"],
    "cash": ["us-gaap_CashAndCashEquivalentsAtCarryingValue"],
    "short_term_investments": [
        "us-gaap_MarketableSecuritiesCurrent",
        "us-gaap_ShortTermInvestments",
        "us-gaap_AvailableForSaleSecuritiesDebtSecuritiesCurrent",
    ],
    "long_term_debt": ["us-gaap_LongTermDebtNoncurrent"],
    "current_debt": ["us-gaap_LongTermDebtCurrent"],
    "commercial_paper": ["us-gaap_CommercialPaper"],
    "short_term_borrowings": ["us-gaap_ShortTermBorrowings"],
}

_PERIOD_COL = re.compile(r"^(\d{4}-\d{2}-\d{2})")


def _latest_period_col(df_dict):
    """Newest 'YYYY-MM-DD[ (FY)]' column name in a statement to_dict(), or None."""
    dated = [(m.group(1), col) for col in df_dict
             if (m := _PERIOD_COL.match(str(col)))]
    if not dated:
        return None
    return max(dated)[1]


def _concept_value(df_dict, aliases):
    """
    First consolidated (non-dimension, non-abstract) value for any alias, in
    alias priority order, from the latest period column. XBRL statements
    repeat a concept once per segment breakdown (product/geography); the
    'dimension' flag separates the consolidated row from breakdowns.
    """
    period = _latest_period_col(df_dict)
    concepts = df_dict.get("concept")
    if not period or not isinstance(concepts, dict):
        return None
    values = df_dict.get(period, {})
    dimensions = df_dict.get("dimension", {})
    abstracts = df_dict.get("abstract", {})
    for alias in aliases:
        for idx, concept in concepts.items():
            if concept != alias:
                continue
            if dimensions.get(idx) or abstracts.get(idx):
                continue
            val = values.get(idx)
            try:
                f = float(val)
            except (TypeError, ValueError):
                continue
            if f == f:  # not NaN
                return f
    return None


def extract_financials_from_statements(fin_result):
    """
    Map an mcp.edgar.financials.get_financials() result to the map's flat
    financial fields via us-gaap concept extraction. Missing concepts stay
    None — never fabricated. Pure (no network), unit-tested.
    """
    out = {
        "revenue": None, "ebitda": None, "operating_cashflow": None,
        "free_cashflow": None, "total_cash": None, "total_debt": None,
        "period": None,
    }
    data = (fin_result or {}).get("data") or {}
    inc = ((data.get("income_statement") or {}).get("data"))
    cf = ((data.get("cash_flow") or {}).get("data"))
    bs = ((data.get("balance_sheet") or {}).get("data"))

    def val(df_dict, key):
        return _concept_value(df_dict, CONCEPT_ALIASES[key]) if isinstance(df_dict, dict) else None

    out["revenue"] = val(inc, "revenue")

    op_inc = val(inc, "operating_income")
    dep_am = val(cf, "depreciation_amortization")
    if op_inc is not None and dep_am is not None:
        out["ebitda"] = op_inc + dep_am

    ocf = val(cf, "operating_cashflow")
    capex = val(cf, "capital_expenditures")
    out["operating_cashflow"] = ocf
    if ocf is not None and capex is not None:
        # Capex is an outflow; statements present it with either sign
        # depending on the concept's weight. FCF must always subtract it.
        out["free_cashflow"] = ocf - abs(capex)

    cash = val(bs, "cash")
    if cash is not None:
        out["total_cash"] = cash + (val(bs, "short_term_investments") or 0.0)

    debt_parts = [val(bs, k) for k in
                  ("long_term_debt", "current_debt", "commercial_paper", "short_term_borrowings")]
    present = [d for d in debt_parts if d is not None]
    if present:
        out["total_debt"] = sum(present)

    if isinstance(inc, dict):
        col = _latest_period_col(inc)
        if col:
            out["period"] = _PERIOD_COL.match(str(col)).group(1)
    return out


def build_insider_holders(rows, limit=12):
    """
    Collapse Form 4 transaction rows (newest first, from
    forms_insider.get_insider_transactions) into one insider ring entry per
    person: name, title, last transaction direction. Form 4 rows carry
    per-transaction shares, not total holdings, so 'shares' honestly stays 0
    (the map does not render it). Pure, unit-tested.
    """
    seen = set()
    insiders = []
    for row in rows or []:
        name = row.get("insider")
        if not name:
            continue
        key = " ".join(str(name).upper().split())
        if key in seen:
            continue
        seen.add(key)
        tx = str(row.get("transaction_type") or "").lower()
        code = str(row.get("code") or "").upper()
        if "purchase" in tx or "buy" in tx or code in ("P", "M"):
            direction = "buy"
        elif "sale" in tx or "sell" in tx or code in ("S", "F"):
            direction = "sell"
        else:
            direction = ""
        insiders.append({
            "name": str(name),
            "title": _safe_str(row.get("position")),
            "shares": 0.0,
            "percentage": 0.0,
            "last_transaction": direction,
            "filing_date": _safe_str(row.get("filing_date")),
        })
        if len(insiders) >= limit:
            break
    return insiders


def build_institutional_holders(holder_rows):
    """
    Map forms_13dg.get_major_holders rows to the map's institutional-holder
    shape, keeping filing provenance. Numbers absent from a legacy text-only
    filing stay null — the UI shows provenance instead. Pure, unit-tested.
    """
    holders = []
    for row in holder_rows or []:
        name = row.get("name")
        if not name:
            continue
        holders.append({
            "name": str(name),
            "shares": row.get("shares"),
            "value": None,
            "percentage": row.get("percent_of_class"),
            "change_percent": 0.0,
            "fund_family": "",
            "type": "institutional",
            "source_form": _safe_str(row.get("form")),
            "filing_date": _safe_str(row.get("filing_date")),
        })
    return holders


def fetch_edgar_sections(ticker: str) -> dict:
    """
    Fetch the EDGAR-sourced sections. Each section fails independently: a
    failure leaves that section empty and records the reason in
    'section_errors'. No yfinance anywhere in this path.
    """
    out = {
        "financials": {},
        "insider_holders": [],
        "institutional_holders": [],
        "section_errors": {},
    }
    try:
        from mcp.edgar import base as edgar_base
        from mcp.edgar import financials as edgar_financials
        from mcp.edgar import forms_insider as edgar_insider
        from mcp.edgar import forms_13dg as edgar_13dg
    except ImportError as e:
        out["section_errors"]["edgar"] = f"mcp.edgar unavailable: {e}"
        return out

    try:
        edgar_base.initialize_edgar()
    except Exception as e:
        out["section_errors"]["edgar"] = f"initialize_edgar failed: {e}"
        return out

    fin_result = edgar_financials.get_financials(ticker)
    if fin_result.get("success"):
        out["financials"] = extract_financials_from_statements(fin_result)
    else:
        out["section_errors"]["financials"] = str(
            (fin_result.get("error") or {}).get("error", "get_financials failed"))

    ins_result = edgar_insider.get_insider_transactions(ticker, limit=15)
    if ins_result.get("success"):
        out["insider_holders"] = build_insider_holders(ins_result.get("data"))
    else:
        out["section_errors"]["insiders"] = str(
            (ins_result.get("error") or {}).get("error", "get_insider_transactions failed"))

    hold_result = edgar_13dg.get_major_holders(ticker, limit=10, max_scan=25)
    if hold_result.get("success"):
        out["institutional_holders"] = build_institutional_holders(hold_result.get("data"))
    else:
        out["section_errors"]["institutional"] = str(
            (hold_result.get("error") or {}).get("error", "get_major_holders failed"))

    return out


# ═══════════════════════════════════════════════════════════════════════════
# PEER CANDIDATES (curated list only — metrics fetched natively in C++)
# ═══════════════════════════════════════════════════════════════════════════

INDUSTRY_PEERS = {
    "Technology":        ["AAPL", "MSFT", "GOOGL", "META", "AMZN", "NVDA", "CRM", "ORCL", "ADBE"],
    "Software":          ["MSFT", "CRM", "ORCL", "ADBE", "NOW", "INTU", "SNOW", "PLTR"],
    "Semiconductors":    ["NVDA", "AMD", "INTC", "AVGO", "QCOM", "TXN", "MU", "AMAT"],
    "Electric Vehicles": ["TSLA", "RIVN", "LCID", "NIO", "XPEV", "LI"],
    "Auto Manufacturers":["TSLA", "F", "GM", "TM", "HMC", "STLA"],
    "Banks":             ["JPM", "BAC", "WFC", "C", "GS", "MS", "USB"],
    "Pharmaceuticals":   ["JNJ", "PFE", "MRK", "ABBV", "LLY", "BMY", "AMGN"],
    "Oil & Gas":         ["XOM", "CVX", "COP", "EOG", "SLB", "PSX"],
    "Retail":            ["WMT", "COST", "TGT", "HD", "LOW", "AMZN"],
    "Payments":          ["V", "MA", "PYPL", "SQ", "FIS", "FISV"],
    "Cloud Computing":   ["AMZN", "MSFT", "GOOGL", "CRM", "SNOW", "NOW"],
    "Telecom":           ["T", "VZ", "TMUS", "CMCSA"],
    "Healthcare":        ["UNH", "CVS", "HCA", "MCK", "ABC"],
    "Insurance":         ["BRK-B", "MET", "PRU", "AFL", "AIG"],
    "Real Estate":       ["AMT", "PLD", "CCI", "EQIX", "SPG"],
    "Consumer":          ["PG", "KO", "PEP", "UL", "CL", "MCD", "SBUX"],
    "Airlines":          ["DAL", "UAL", "AAL", "LUV", "ALK"],
    "Defense":           ["LMT", "RTX", "NOC", "GD", "BA"],
    "Energy":            ["NEE", "DUK", "SO", "AEP", "D"],
}


def peer_candidates(ticker: str, industry: str, sector: str) -> list:
    """Curated peer tickers for the sector/industry (no network). Pure."""
    for key, peers_list in INDUSTRY_PEERS.items():
        if key.lower() in (industry or "").lower() or key.lower() in (sector or "").lower():
            return [p for p in peers_list if p != ticker.upper()][:8]
    return [p for p in ["AAPL", "MSFT", "GOOGL", "AMZN", "META"] if p != ticker.upper()][:5]


# ═══════════════════════════════════════════════════════════════════════════
# YFINANCE SECTIONS (no EDGAR/native replacement yet — see issue #83)
# ═══════════════════════════════════════════════════════════════════════════

def fetch_company_data(ticker: str) -> dict:
    result = {
        "company": {},
        "governance": {},
        "technicals": {},
        "short_interest": {},
        "enterprise": {},
        "margins": {},
        "analyst_targets": {},
        "recommendations_summary": [],
        "upgrades_downgrades": [],
        "officers": [],
        "institutional_holders": [],
        "mutualfund_holders": [],
        "insider_holders": [],
        "peer_tickers": [],
        "calendar": {},
        "supply_chain": [],
        "section_errors": {},
        "data_quality": 0,
    }

    quality = 0

    try:
        import yfinance as yf
    except ImportError:
        yf = None
        result["section_errors"]["yfinance"] = "yfinance not installed. Run: pip install yfinance"

    info = {}
    stock = None
    if yf is not None:
        try:
            stock = yf.Ticker(ticker)
            info = stock.info or {}
        except Exception as e:
            result["section_errors"]["yfinance"] = str(e)

    # ── Company Info (profile + market ratios — yfinance; statement values
    #    revenue/ebitda/cashflows/cash/debt are EDGAR XBRL, merged below) ────
    result["company"] = {
        "ticker": ticker.upper(),
        "name": info.get("longName", info.get("shortName", ticker)),
        "sector": info.get("sector", ""),
        "industry": info.get("industry", ""),
        "website": info.get("website", ""),
        "description": (info.get("longBusinessSummary") or "")[:300],
        "employees": _safe_int(info.get("fullTimeEmployees")),
        "country": info.get("country", ""),
        "exchange": info.get("exchange", ""),
        "currency": info.get("currency", "USD"),
        "market_cap": _safe_float(info.get("marketCap")),
        "current_price": _safe_float(info.get("currentPrice", info.get("regularMarketPrice"))),
        "previous_close": _safe_float(info.get("previousClose")),
        "day_change_pct": _safe_float(info.get("regularMarketChangePercent")),
        "pe_ratio": _safe_float(info.get("trailingPE")),
        "forward_pe": _safe_float(info.get("forwardPE")),
        "price_to_book": _safe_float(info.get("priceToBook")),
        "roe": _safe_float(info.get("returnOnEquity")),
        "roa": _safe_float(info.get("returnOnAssets")),
        "revenue_growth": _safe_float(info.get("revenueGrowth")),
        "earnings_growth": _safe_float(info.get("earningsGrowth")),
        "profit_margins": _safe_float(info.get("profitMargins")),
        "revenue": 0.0,
        "ebitda": 0.0,
        "free_cashflow": 0.0,
        "operating_cashflow": 0.0,
        "total_cash": 0.0,
        "total_debt": 0.0,
        "insider_percent": _safe_float(info.get("heldPercentInsiders")),
        "institutional_percent": _safe_float(info.get("heldPercentInstitutions")),
        "recommendation": info.get("recommendationKey", ""),
        "recommendation_mean": _safe_float(info.get("recommendationMean")),
        "target_high": _safe_float(info.get("targetHighPrice")),
        "target_low": _safe_float(info.get("targetLowPrice")),
        "target_mean": _safe_float(info.get("targetMeanPrice")),
        "target_median": _safe_float(info.get("targetMedianPrice")),
        "analyst_count": _safe_int(info.get("numberOfAnalystOpinions")),
        "dividend_yield": _safe_float(info.get("dividendYield")),
        "payout_ratio": _safe_float(info.get("payoutRatio")),
        "trailing_eps": _safe_float(info.get("trailingEps")),
        "forward_eps": _safe_float(info.get("forwardEps")),
        "shares_outstanding": _safe_float(info.get("sharesOutstanding")),
    }
    if info:
        quality += 25

    # ── Governance Risk ───────────────────────────────────────────────────
    overall = _safe_int(info.get("overallRisk"))
    if overall > 0:
        quality += 5
        result["governance"] = {
            "audit_risk": _safe_int(info.get("auditRisk")),
            "board_risk": _safe_int(info.get("boardRisk")),
            "compensation_risk": _safe_int(info.get("compensationRisk")),
            "shareholder_rights_risk": _safe_int(info.get("shareHolderRightsRisk")),
            "overall_risk": overall,
        }

    # ── Technicals ────────────────────────────────────────────────────────
    result["technicals"] = {
        "fifty_two_week_high": _safe_float(info.get("fiftyTwoWeekHigh")),
        "fifty_two_week_low": _safe_float(info.get("fiftyTwoWeekLow")),
        "fifty_day_avg": _safe_float(info.get("fiftyDayAverage")),
        "two_hundred_day_avg": _safe_float(info.get("twoHundredDayAverage")),
        "beta": _safe_float(info.get("beta")),
        "week52_change_pct": _safe_float(info.get("52WeekChange")),
        "sp500_52wk_change": _safe_float(info.get("SandP52WeekChange")),
        "avg_volume": _safe_int(info.get("averageVolume")),
        "avg_volume_10d": _safe_int(info.get("averageDailyVolume10Day")),
    }
    if info:
        quality += 5

    # ── Short Interest ────────────────────────────────────────────────────
    shares_short = _safe_float(info.get("sharesShort"))
    if shares_short > 0:
        quality += 5
        result["short_interest"] = {
            "shares_short": shares_short,
            "short_ratio": _safe_float(info.get("shortRatio")),
            "short_pct_float": _safe_float(info.get("shortPercentOfFloat")),
            "float_shares": _safe_float(info.get("floatShares")),
        }

    # ── Enterprise Metrics ────────────────────────────────────────────────
    result["enterprise"] = {
        "enterprise_value": _safe_float(info.get("enterpriseValue")),
        "ev_to_revenue": _safe_float(info.get("enterpriseToRevenue")),
        "ev_to_ebitda": _safe_float(info.get("enterpriseToEbitda")),
        "peg_ratio": _safe_float(info.get("trailingPegRatio")),
        "price_to_sales": _safe_float(info.get("priceToSalesTrailing12Months")),
        "book_value": _safe_float(info.get("bookValue")),
    }

    # ── Margins ───────────────────────────────────────────────────────────
    result["margins"] = {
        "gross": _safe_float(info.get("grossMargins")),
        "operating": _safe_float(info.get("operatingMargins")),
        "ebitda": _safe_float(info.get("ebitdaMargins")),
        "net": _safe_float(info.get("profitMargins")),
        "debt_to_equity": _safe_float(info.get("debtToEquity")),
        "current_ratio": _safe_float(info.get("currentRatio")),
        "quick_ratio": _safe_float(info.get("quickRatio")),
    }

    # ── Analyst Price Targets ─────────────────────────────────────────────
    if stock is not None:
        try:
            apt = stock.analyst_price_targets
            if isinstance(apt, dict) and apt:
                quality += 5
                result["analyst_targets"] = {
                    "current": _safe_float(apt.get("current")),
                    "high": _safe_float(apt.get("high")),
                    "low": _safe_float(apt.get("low")),
                    "mean": _safe_float(apt.get("mean")),
                    "median": _safe_float(apt.get("median")),
                }
        except Exception:
            pass

        # ── Recommendations Summary ───────────────────────────────────────
        try:
            rec = stock.recommendations_summary
            if rec is not None and not rec.empty:
                quality += 5
                for _, row in rec.iterrows():
                    result["recommendations_summary"].append({
                        "period": _safe_str(row.get("period")),
                        "strong_buy": _safe_int(row.get("strongBuy")),
                        "buy": _safe_int(row.get("buy")),
                        "hold": _safe_int(row.get("hold")),
                        "sell": _safe_int(row.get("sell")),
                        "strong_sell": _safe_int(row.get("strongSell")),
                    })
        except Exception:
            pass

        # ── Upgrades / Downgrades ─────────────────────────────────────────
        try:
            ud = stock.upgrades_downgrades
            if ud is not None and not ud.empty:
                quality += 5
                for idx, row in ud.head(8).iterrows():
                    result["upgrades_downgrades"].append({
                        "date": _safe_str(idx)[:10],
                        "firm": _safe_str(row.get("Firm")),
                        "to_grade": _safe_str(row.get("ToGrade")),
                        "from_grade": _safe_str(row.get("FromGrade")),
                        "action": _safe_str(row.get("Action")),
                        "price_target": _safe_float(row.get("currentPriceTarget")),
                        "prior_target": _safe_float(row.get("priorPriceTarget")),
                    })
        except Exception:
            pass

        # ── Company Officers ──────────────────────────────────────────────
        try:
            officers = info.get("companyOfficers", [])
            if officers:
                quality += 5
                for o in officers[:8]:
                    result["officers"].append({
                        "name": _safe_str(o.get("name")),
                        "title": _safe_str(o.get("title")),
                        "total_pay": _safe_int(o.get("totalPay")),
                        "year_born": _safe_int(o.get("yearBorn")),
                    })
        except Exception:
            pass

        # ── Calendar (Earnings, Dividends) ────────────────────────────────
        try:
            cal = stock.calendar
            if isinstance(cal, dict) and cal:
                quality += 5
                result["calendar"] = {
                    "earnings_date": _safe_str(cal.get("Earnings Date", [None])[0] if isinstance(cal.get("Earnings Date"), list) else cal.get("Earnings Date")),
                    "earnings_avg": _safe_float(cal.get("Earnings Average")),
                    "earnings_low": _safe_float(cal.get("Earnings Low")),
                    "earnings_high": _safe_float(cal.get("Earnings High")),
                    "revenue_avg": _safe_float(cal.get("Revenue Average")),
                    "revenue_low": _safe_float(cal.get("Revenue Low")),
                    "revenue_high": _safe_float(cal.get("Revenue High")),
                    "ex_dividend_date": _safe_str(cal.get("Ex-Dividend Date")),
                    "dividend_date": _safe_str(cal.get("Dividend Date")),
                }
        except Exception:
            pass

        # ── Mutual Fund Holders ───────────────────────────────────────────
        try:
            mf = stock.mutualfund_holders
            if mf is not None and not mf.empty:
                quality += 5
                for _, row in mf.head(10).iterrows():
                    pct = _safe_float(row.get("pctHeld", 0))
                    if pct < 1:
                        pct *= 100
                    holder = {
                        "name": _safe_str(row.get("Holder")),
                        "shares": _safe_float(row.get("Shares")),
                        "value": _safe_float(row.get("Value")),
                        "percentage": pct,
                        "change_percent": _safe_float(row.get("pctChange", 0)) * 100,
                        "fund_family": "",
                        "type": "mutualfund",
                    }
                    if holder["name"]:
                        result["mutualfund_holders"].append(holder)
        except Exception:
            pass

    # ── EDGAR sections: financials, insiders, major holders ───────────────
    edgar = fetch_edgar_sections(ticker)
    result["section_errors"].update(edgar["section_errors"])

    fin = edgar["financials"]
    if fin:
        for key in ("revenue", "ebitda", "free_cashflow",
                    "operating_cashflow", "total_cash", "total_debt"):
            result["company"][key] = _safe_float(fin.get(key))
        result["financials_provenance"] = {
            "source": "SEC EDGAR XBRL (10-K)",
            "period": _safe_str(fin.get("period")),
        }
        if fin.get("revenue") is not None:
            quality += 10

    result["insider_holders"] = edgar["insider_holders"]
    if result["insider_holders"]:
        quality += 5

    result["institutional_holders"] = edgar["institutional_holders"]
    if result["institutional_holders"]:
        quality += 10

    # ── Peer candidates (metrics fetched natively by the terminal) ────────
    result["peer_tickers"] = peer_candidates(
        ticker, result["company"].get("industry", ""), result["company"].get("sector", ""))

    result["data_quality"] = min(quality, 100)
    return result


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"error": "Usage: relationship_map.py <TICKER>"}))
        sys.exit(1)

    ticker = sys.argv[1].strip().upper()
    data = fetch_company_data(ticker)
    print(json.dumps(data, default=str))
