# Skill: Thesis Monitor

Use when the user wants to track an investment thesis, pressure-test a view, or ask
"what would change my mind?" The goal is to make assumptions explicit and search for
both confirming and disconfirming evidence.

Use app tools such as `get_equity_news`, `edgar_get_financials`, `edgar_10k_sections`,
`edgar_search_filings`, `edgar_get_filing_text`, `get_quote`, `get_notes`,
`report_add_component`, and any relevant tools discovered with `tool_list`.

## Method
1. **Extract the thesis.** If the user supplied a thesis, decompose it into 3-6
   testable assumptions. If not, draft a neutral bull/base/bear thesis from available
   fundamentals, disclosures, and market narrative.
2. **Map evidence.** For each assumption, collect current evidence from filings,
   financials, news, price action, and user notes. Include source and date.
3. **Look for disconfirmation first.** Search for facts that weaken or invalidate
   the thesis: margin pressure, demand slowdown, leverage, dilution, regulation,
   competitive change, management language, or price action inconsistent with the
   narrative.
4. **Set monitors.** Define measurable watch items: KPIs, filing sections, news
   keywords, macro series, price levels, earnings call topics, or risk thresholds.
5. **Update status.** Mark the thesis as strengthening, unchanged, weakening, or
   broken. Explain why and identify the next data point that matters most.

## Output
Build a Report Builder thesis update with: thesis summary, assumption/evidence table,
disconfirming evidence, monitoring checklist, status, and source log. Avoid trade
recommendations; focus on whether the thesis is better or worse supported by data.
