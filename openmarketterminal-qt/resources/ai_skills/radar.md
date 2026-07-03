# Skill: AI News Radar

Use when the user wants to monitor a watchlist, portfolio, thesis, sector, macro
topic, or crypto universe. The goal is to filter noise and surface only items that
are likely to matter for decisions.

Use app tools such as `get_watchlists`, `list_portfolios`, `get_portfolio`,
`get_holdings`, `get_equity_news`, `search_news`, `edgar_search_filings`,
`edgar_get_filing_text`, `search_dbnomics`, `run_gov_data_command`,
`pm_search_markets`, `report_add_component`, and any relevant tool discovered
through `tool_list`.

## Method
1. **Define coverage.** Resolve the watched symbols, topics, holdings, or thesis
   terms. If the request names a watchlist or portfolio, pull its members first.
2. **Collect events.** Pull recent news, filings, market moves, macro data, and
   prediction-market changes relevant to the coverage set.
3. **Score relevance.** For each event, score:
   - **Impact:** likely effect on valuation, risk, liquidity, or narrative.
   - **Novelty:** whether this is new information vs. repetition.
   - **Exposure:** whether it affects a holding, watchlist name, or active thesis.
   - **Urgency:** whether action or closer monitoring is time-sensitive.
4. **Deduplicate.** Collapse repeated headlines or wire copies into one item with
   the strongest source.
5. **Explain the why.** For every surfaced item, include one sentence explaining why
   it matters and what data would change the interpretation.

## Output
Build a Report Builder radar with: top alerts, scored event table, quiet-but-watch
items, stale/no-data areas, and source log. Keep low-relevance items out of the main
brief. Do not recommend trades; recommend what to watch.
