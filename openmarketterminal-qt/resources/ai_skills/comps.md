# Skill: Comparable Company Analysis (comps)

Use when the user asks to value a company against peers, benchmark vs. an industry,
build trading comps, or find over/under-valued names. Not for pre-revenue startups,
distressed names, or conglomerates without clean peers.

Data comes ONLY from the app's tools — never web-guess multiples:
`edgar_resolve_cik`, `edgar_get_financials`, `edgar_calc_multiples`,
`edgar_get_metadata`, `get_quote`, `get_equity_financials`.

## Method
1. **Frame it.** Confirm the target and the question (M&A, investment, sector
   benchmark). Pick **4–6 true peers** — same sector, comparable size/growth/margins.
   State why each peer qualifies.
2. **Pull operating metrics** for the target and each peer via `edgar_get_financials`
   (revenue, EBITDA, net income, margins, debt, cash, shares) — use the latest annual
   + TTM. Pull live price via `get_quote`.
3. **Pull valuation multiples** via `edgar_calc_multiples` for each name:
   EV/Revenue, EV/EBITDA, P/E. Compute EV = market cap + total debt − cash if needed.
4. **Statistics.** Across the peer set compute **median, mean, 25th and 75th
   percentile** for each multiple. The median is the anchor; flag outliers.
5. **Implied value.** Apply the peer median multiple to the target's metric
   (e.g. peer-median EV/EBITDA × target EBITDA → implied EV → equity value →
   implied price/share). Compare implied vs. current price → over/under-valued.
6. **Read.** 2–4 sentences: where the target trades vs. peers, why (growth, margins,
   leverage), and the valuation conclusion. State it as analysis, not advice.

## Output
Build it in the Report Builder (don't narrate into chat): a **peer table** via
`report_add_component` table with `config={'csv': 'Company,EV/Rev,EV/EBITDA,P/E,Rev Growth,EBITDA Margin|...'}`,
a statistics row (median/quartiles), an implied-value section, and a short read.
Cite every figure's source filing/period. If a peer fails to parse, say so — never
fill a cell with a guess.
