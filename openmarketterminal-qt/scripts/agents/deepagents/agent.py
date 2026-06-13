"""
agent.py — Full create_deep_agent wiring for OpenMarketTerminal.

Single responsibility:
  - create_agent() factory that wires ALL 16 create_deep_agent params
  - OpenMarketTerminalContext dataclass for user/session injection via context_schema
  - MCP tool wrappers as LangChain BaseTool instances
  - InMemoryStore + InMemoryCache always created
  - MemorySaver checkpointer always on (thread-level state)
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from langchain_core.tools import BaseTool, tool
from langgraph.checkpoint.memory import MemorySaver
from langgraph.store.memory import InMemoryStore
from langgraph.cache.memory import InMemoryCache

from deepagents.graph import resolve_model, GENERAL_PURPOSE_SUBAGENT, BASE_AGENT_PROMPT
from langchain.agents.middleware import InterruptOnConfig

from backends import get_backend
from models import create_model, extract_text
from subagents import get_subagents_for_type

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# OpenMarketTerminalContext — injected into every agent invocation via context_schema
# ---------------------------------------------------------------------------

@dataclass
class OpenMarketTerminalContext:
    """
    Typed context passed to every deep agent via context_schema / invoke(context=...).

    Gives agents access to session and user state without polluting the message stream.
    """
    user_id:      str = ""
    session_id:   str = ""
    agent_type:   str = "general"
    portfolio_id: str = ""
    watchlist:    list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# MCP tool wrappers — LangChain BaseTool instances wrapping OpenMarketTerminal capabilities
# ---------------------------------------------------------------------------

import json as _json
import re as _re


def _extract_tickers(query: str):
    """Pull candidate ticker symbols out of a free-text query."""
    toks = _re.findall(r"\b[A-Z][A-Z.\-]{0,5}\b", query or "")
    stop = {"USD", "OHLCV", "CEO", "IPO", "ETF", "GDP", "CPI", "FED", "US", "EPS",
            "PE", "AI", "Q1", "Q2", "Q3", "Q4", "YTD", "NYSE", "NASDAQ", "EU", "UK"}
    cands = [t for t in toks if t not in stop]
    if not cands and query and query.strip():
        cands = [query.strip().split()[0].upper()]
    return cands[:5]


def _make_market_data_tool() -> BaseTool:
    @tool
    def market_data(query: str) -> str:
        """
        Fetch LIVE market data for a symbol or query.
        Input: symbol name, query like 'AAPL price', 'NVDA quote', 'MSFT OHLCV'.
        Returns: JSON string with real current market data from Yahoo Finance.
        """
        try:
            import yfinance as yf
        except Exception:
            return _json.dumps({"available": False, "error": "yfinance not available in this environment."})
        tickers = _extract_tickers(query)
        if not tickers:
            return _json.dumps({"available": False, "error": "Could not identify a ticker symbol in the query."})
        out = {}
        for sym in tickers:
            try:
                t = yf.Ticker(sym)
                hist = t.history(period="5d", auto_adjust=True)
                if hist is None or hist.empty:
                    continue
                last = float(hist["Close"].iloc[-1])
                prev = float(hist["Close"].iloc[-2]) if len(hist) > 1 else None
                out[sym] = {
                    "last_price": round(last, 4),
                    "prev_close": round(prev, 4) if prev else None,
                    "change_pct": round((last / prev - 1) * 100, 2) if prev else None,
                    "day_high": round(float(hist["High"].iloc[-1]), 4),
                    "day_low": round(float(hist["Low"].iloc[-1]), 4),
                    "volume": int(hist["Volume"].iloc[-1]),
                    "as_of": str(hist.index[-1].date()),
                }
            except Exception:
                continue
        if not out:
            return _json.dumps({"available": False, "error": f"No market data found for {tickers}."})
        return _json.dumps({"available": True, "source": "yahoo_finance", "data": out})
    return market_data


def _make_portfolio_tool() -> BaseTool:
    @tool
    def portfolio_data(query: str) -> str:
        """
        Access the user's portfolio positions, P&L, and allocation.
        Input: query like 'current positions', 'portfolio P&L', 'sector allocation'.
        Returns: JSON string with portfolio data.
        """
        # Honesty: a standalone research agent has no access to the user's live
        # portfolio (it lives in the app's session/database, not here). Report that
        # plainly rather than fabricate positions.
        return _json.dumps({
            "available": False,
            "error": ("portfolio_data requires the running app's portfolio session and is not "
                      "accessible to this standalone agent. No positions were returned."),
        })
    return portfolio_data


def _make_news_tool() -> BaseTool:
    @tool
    def financial_news(query: str) -> str:
        """
        Fetch recent REAL financial news for a company or symbol.
        Input: company name or symbol like 'AAPL earnings', 'NVDA', 'Tesla'.
        Returns: JSON string with real recent headlines from Yahoo Finance.
        """
        try:
            import yfinance as yf
        except Exception:
            return _json.dumps({"available": False, "error": "yfinance not available in this environment."})
        tickers = _extract_tickers(query)
        if not tickers:
            return _json.dumps({"available": False, "error": "Could not identify a ticker symbol for news."})
        items = []
        for sym in tickers[:2]:
            try:
                for n in (yf.Ticker(sym).news or [])[:5]:
                    c = n.get("content", n)  # newer yfinance nests under 'content'
                    title = c.get("title")
                    if not title:
                        continue
                    prov = c.get("provider")
                    canon = c.get("canonicalUrl")
                    items.append({
                        "ticker": sym,
                        "title": title,
                        "publisher": prov.get("displayName") if isinstance(prov, dict) else c.get("publisher"),
                        "published": c.get("pubDate") or c.get("providerPublishTime"),
                        "url": canon.get("url") if isinstance(canon, dict) else c.get("link"),
                    })
            except Exception:
                continue
        if not items:
            return _json.dumps({"available": False, "error": f"No recent news found for {tickers}."})
        return _json.dumps({"available": True, "source": "yahoo_finance", "articles": items})
    return financial_news


# Market-based macro proxies reachable via Yahoo (yields, vol, FX, commodities, indices).
_MACRO_PROXIES = {
    "^TNX": ("10-Year Treasury Yield", ["10 year", "10-year", "treasury", "yield curve", "yields", "tnx"]),
    "^FVX": ("5-Year Treasury Yield", ["5 year", "5-year", "fvx"]),
    "^IRX": ("13-Week T-Bill Rate", ["13 week", "t-bill", "short rate", "irx", "fed funds", "fed rate"]),
    "^VIX": ("CBOE Volatility Index (VIX)", ["vix", "volatility", "fear"]),
    "DX-Y.NYB": ("US Dollar Index (DXY)", ["dollar", "dxy", "usd index"]),
    "GC=F": ("Gold Futures", ["gold"]),
    "CL=F": ("Crude Oil Futures (WTI)", ["oil", "crude", "wti"]),
    "^GSPC": ("S&P 500 Index", ["s&p", "sp500", "s and p"]),
    "^IXIC": ("Nasdaq Composite", ["nasdaq"]),
    "^DJI": ("Dow Jones Industrial Average", ["dow"]),
}


def _make_economics_tool() -> BaseTool:
    @tool
    def economics_data(query: str) -> str:
        """
        Fetch LIVE market-based macro indicators: Treasury yields, the VIX, the US
        dollar index, gold/oil, and major equity indices.
        Input: indicator like 'yield curve', '10-year treasury', 'VIX', 'dollar', 'gold', 'oil'.
        Returns: JSON with real current values. Official statistics (CPI, GDP,
        unemployment) require a FRED API key and are not available here.
        """
        try:
            import yfinance as yf
        except Exception:
            return _json.dumps({"available": False, "error": "yfinance not available in this environment."})
        q = (query or "").lower()
        matched = [(sym, name) for sym, (name, kws) in _MACRO_PROXIES.items()
                   if any(k in q for k in kws)]
        if not matched:
            official = any(k in q for k in ("cpi", "inflation", "gdp", "unemployment", "payroll", "jobs"))
            return _json.dumps({
                "available": False,
                "error": ("No market-based macro proxy matched this query."
                          + (" Official statistics (CPI/GDP/unemployment) require a FRED API key, "
                             "which is not configured for this agent." if official else "")),
                "supported": [name for _, (name, _k) in _MACRO_PROXIES.items()],
            })
        out = {}
        for sym, name in matched:
            try:
                hist = yf.Ticker(sym).history(period="5d")
                if hist is None or hist.empty:
                    continue
                last = float(hist["Close"].iloc[-1])
                prev = float(hist["Close"].iloc[-2]) if len(hist) > 1 else None
                out[name] = {
                    "value": round(last, 4),
                    "change_pct": round((last / prev - 1) * 100, 2) if prev else None,
                    "as_of": str(hist.index[-1].date()),
                }
            except Exception:
                continue
        if not out:
            return _json.dumps({"available": False, "error": "Could not fetch the matched macro indicators."})
        return _json.dumps({"available": True, "source": "yahoo_finance", "note": "market-based proxies", "data": out})
    return economics_data


def _build_tools() -> list[BaseTool]:
    """Build the list of MCP-wrapper tools for the agent."""
    return [
        _make_market_data_tool(),
        _make_portfolio_tool(),
        _make_news_tool(),
        _make_economics_tool(),
    ]


# ---------------------------------------------------------------------------
# System prompt
# ---------------------------------------------------------------------------

_OpenMarketTerminal_SYSTEM_PROMPT = """You are a Deep Agent for OpenMarketTerminal, an institutional-grade financial intelligence platform.

You have access to:
- Financial market data (prices, OHLCV, order books)
- Portfolio positions and P&L
- Financial news and research
- Macroeconomic indicators
- 1300+ analytics Python scripts in /scripts/
- Specialist subagents for research, analysis, trading, risk, and reporting

Standards:
- Apply CFA Level III analytical standards
- Always quantify uncertainty and data recency
- Distinguish between verified facts and estimates
- Consider risk in every recommendation
- Output structured, professional analysis

When delegating to subagents, be specific about what you need from each one.
"""

# ---------------------------------------------------------------------------
# Main factory
# ---------------------------------------------------------------------------

# Module-level shared store and cache (one per process)
_store = InMemoryStore()
_cache = InMemoryCache()


def create_agent(
    model: Any,
    config: dict[str, Any],
    scripts_dir: str | None = None,
) -> Any:
    """
    Build and return a compiled DeepAgent (CompiledStateGraph).

    Args:
        model       : BaseChatModel from models.create_model()
        config      : Full OpenMarketTerminal LLM config dict (used for agent_type, backend_mode, etc.)
        scripts_dir : Absolute path to scripts/ directory. Auto-detected if None.

    Returns:
        CompiledStateGraph — call .invoke() or .stream() on it.
    """
    from deepagents import create_deep_agent

    # Resolve scripts directory
    if scripts_dir is None:
        scripts_dir = _find_scripts_dir()

    agent_type   = config.get("agent_type",    "general")
    backend_mode = config.get("backend_mode",  "composite")
    interrupt_on = config.get("interrupt_on",  {"execute": True, "write_file": True})
    debug        = config.get("debug",         False)

    # Resolve model via library utility (handles str format like "openai:gpt-4o" too)
    resolved_model = resolve_model(model)

    # Backend
    backend = get_backend(
        mode=backend_mode,
        scripts_dir=scripts_dir,
        store=_store,
    )

    # Subagents
    subagents = get_subagents_for_type(agent_type)

    # Memory path — only include if AGENTS.md exists
    memory_dir  = Path(__file__).parent / "memory" / "AGENTS.md"
    memory_sources = ["/memory/AGENTS.md"] if memory_dir.exists() else None

    # Checkpointer
    checkpointer = MemorySaver()

    # Structured response format (opt-in via config)
    response_format = _build_response_format(config)

    # Build typed interrupt_on using InterruptOnConfig
    typed_interrupt: dict | None = None
    if interrupt_on:
        typed_interrupt = {
            tool: InterruptOnConfig(allowed_decisions=["approve", "edit", "decline"])
            if isinstance(v, bool) and v else v
            for tool, v in interrupt_on.items()
        }

    compiled = create_deep_agent(
        model=resolved_model,
        tools=_build_tools(),
        system_prompt=_OpenMarketTerminal_SYSTEM_PROMPT,
        middleware=(),                    # library builds full stack internally
        subagents=subagents,
        skills=None,                      # no skills defined yet — add when SKILL.md files exist
        memory=memory_sources,
        response_format=response_format,
        context_schema=OpenMarketTerminalContext,
        checkpointer=checkpointer,
        store=_store,
        backend=backend,
        interrupt_on=typed_interrupt,
        debug=debug,
        name=f"openmarketterminal-{agent_type}-agent",
        cache=_cache,
    )

    logger.info(
        "Created openmarketterminal-%s-agent with %d subagents, backend=%s",
        agent_type, len(subagents), backend_mode,
    )
    return compiled


def build_openmarketterminal_context(params: dict[str, Any]) -> OpenMarketTerminalContext:
    """Build a OpenMarketTerminalContext from CLI params dict."""
    ctx = params.get("context", {}) or {}
    return OpenMarketTerminalContext(
        user_id=      ctx.get("user_id",      ""),
        session_id=   ctx.get("session_id",   ""),
        agent_type=   params.get("agent_type", "general"),
        portfolio_id= ctx.get("portfolio_id", ""),
        watchlist=    ctx.get("watchlist",    []),
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _find_scripts_dir() -> str | None:
    """Auto-detect the scripts/ directory relative to this file."""
    here = Path(__file__).resolve()
    # scripts/agents/deepagents/agent.py → go up 3 levels to scripts/
    candidate = here.parent.parent.parent
    if candidate.is_dir() and candidate.name == "scripts":
        return str(candidate)
    return None


def _build_response_format(config: dict[str, Any]) -> Any:
    """
    Build a response_format if the caller requests structured output.

    Config key: response_schema — name of a known Pydantic schema.
    Currently supports: "analysis_report"
    Returns None if not requested.
    """
    schema_name = config.get("response_schema")
    if not schema_name:
        return None

    try:
        from langchain.agents.structured_output import ToolStrategy

        if schema_name == "analysis_report":
            from pydantic import BaseModel

            class AnalysisReport(BaseModel):
                executive_summary: str
                key_findings:      list[str]
                risks:             list[str]
                recommendations:   list[str]
                confidence:        str  # "high" | "medium" | "low"

            return ToolStrategy(schema=AnalysisReport)

    except Exception as exc:
        logger.warning("Could not build response_format for '%s': %s", schema_name, exc)

    return None
