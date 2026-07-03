# Skill: AI Risk Review

Use when the user asks "what can hurt me?", "why is my portfolio down?", "risk check",
or asks for a review before placing or sizing a trade. The goal is to make hidden
exposures visible and tie every concern to data.

Use app tools such as `list_portfolios`, `get_portfolio`, `get_holdings`,
`get_quote`, `get_history`, `get_equity_news`, `search_news`,
`edgar_search_filings`, `get_open_orders`, `get_positions`, `live_get_orders`,
`live_get_positions`, `get_watchlists`, `report_add_component`, and relevant tools
discovered through `tool_list`.

## Method
1. **Resolve exposure.** Identify whether the target is an account, portfolio,
   watchlist, symbol, or proposed order. Pull holdings, cash, open orders, and
   positions where available.
2. **Concentration.** Find largest names, sectors, asset classes, currencies,
   venues, and factor-like exposures. Flag any single-name or correlated cluster
   that dominates risk.
3. **Market risk.** Pull recent price moves, volatility, drawdown, liquidity, and
   technical levels. Prefer measured data; label any inference.
4. **Event risk.** Pull news, filings, earnings, macro releases, funding/liquidation
   events, or governance risks that can move the exposure.
5. **Rule check.** Compare against explicit user rules if available: max notional,
   max position size, drawdown limits, leverage, live-trading safety gates, or thesis
   invalidation criteria.
6. **Mitigations.** Offer concrete risk controls: reduce size, hedge, set alerts,
   wait for a catalyst, diversify, or require human approval. Keep these as risk
   controls, not financial advice.

## Output
Build a Report Builder risk note with: exposure summary, risk table ranked by severity,
event watchlist, suggested controls, missing-data warnings, and source log.
