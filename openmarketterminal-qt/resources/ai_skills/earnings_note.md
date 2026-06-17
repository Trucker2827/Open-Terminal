# Skill: Earnings / Filing Note

Use when the user asks for an earnings review, a read on a recent quarter/10-K, or a
short research note on a company's fundamentals and disclosures. Data via the app's
tools only: `edgar_resolve_cik`, `edgar_get_financials`, `edgar_10k_sections`,
`edgar_search_filings`, `edgar_get_filing_text`, `get_quote`/`get_equity_news`.

## Method
1. **Anchor the numbers.** `edgar_get_financials` → revenue, EBITDA, net income,
   margins, debt, cash, shares (latest annual + TTM, and the prior period for YoY).
   Compute YoY/▲ for the headline lines.
2. **Read the disclosure.** `edgar_10k_sections` for MD&A, business, and risk_factors;
   use `edgar_search_filings` to find the latest 10-K/10-Q/8-K and `edgar_get_filing_text`
   for specifics (guidance, segment detail, one-time items). Pull what's material —
   don't dump the filing.
3. **Synthesize.** Identify: (a) what drove the result (segments, margins, volume vs.
   price), (b) balance-sheet/leverage changes, (c) guidance or forward language, and
   (d) the 2–3 risks that actually matter for this name (from risk_factors + MD&A).
4. **Valuation context.** Live price via `get_quote`; optionally pull comps multiples
   via `edgar_calc_multiples` to frame whether the print is already priced in.

## Output
A tight Report Builder note (not a chat dump): a metrics table (this period vs. prior,
via `report_add_component` table `config={'csv':...}`), a "What happened" paragraph, a
"Balance sheet / capital" note, a "Guidance & outlook" note, and a "Key risks" list —
each section with real content, no placeholders. Bold the headline figures
(`Revenue **+22% YoY** to **$96.8B**`). Cite the filing + period for every number.
Present as analysis of what the data shows, not investment advice.
