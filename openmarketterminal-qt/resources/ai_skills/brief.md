# Skill: AI Market Brief

Use when the user asks for a concise read on a ticker, portfolio, watchlist, sector,
crypto asset, macro topic, or market event. The goal is a decision-ready brief with
sources, not a generic chat summary.

Prefer app data tools and local context: `get_quote`, `get_history`,
`get_equity_news`, `search_news`, `edgar_search_filings`, `edgar_get_filing_text`,
`get_portfolio`, `get_holdings`, `get_watchlists`, `search_dbnomics`,
`run_gov_data_command`, `report_add_component`, and related tools discovered with
`tool_list`.

## Method
1. **Scope the target.** Decide whether the target is a symbol, portfolio, watchlist,
   macro topic, or event. If ambiguous, state the assumption and proceed with the
   most likely interpretation.
2. **Market move.** Pull price/quote data and describe the relevant move: today,
   week-to-date, recent range, volume, volatility, or spread where available. Include
   timestamp and source.
3. **Drivers.** Pull recent news, filings, or macro releases. Cluster into 2-4
   drivers. Separate facts from inference. Quote only short snippets when useful.
4. **Context.** Add relevant fundamentals, technical setup, portfolio exposure,
   event calendar, or macro backdrop. Do not fill missing figures with guesses.
5. **What matters next.** Identify catalysts and watch items: earnings, filings,
   policy releases, supply/demand data, support/resistance, funding/liquidation
   zones, or portfolio risk triggers.
6. **Confidence.** Mark each conclusion High / Medium / Low confidence based on
   source quality, freshness, and agreement across data.

## Output
Build a Report Builder note with: headline, market move, driver table, context,
next-watch checklist, and source log. Keep it concise enough to read in under two
minutes. Present as research support, not financial advice.
