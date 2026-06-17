# Skill: Leveraged Buyout (LBO) model

Use when the user asks for an LBO, a sponsor-returns analysis, or "what IRR could a PE
buyer get." Data via the app's tools only: `edgar_resolve_cik`, `edgar_get_financials`
(EBITDA, debt, cash, capex), `edgar_calc_multiples` / comps for entry & exit multiples,
`get_quote`. Confirm each block before building the next.

## Method
1. **Entry.** Entry EV = entry EV/EBITDA multiple × LTM EBITDA (use the current trading
   multiple or a stated purchase multiple). State the multiple and its source.
2. **Sources & Uses.** Uses = entry EV + fees. Sources = new debt (e.g. 4–6× EBITDA,
   respecting leverage capacity) + **sponsor equity = plug** (Uses − Debt − rolled equity).
   Show the table; both sides must tie.
3. **Operating model (5 yr).** Project revenue → EBITDA (margin assumption) → less capex,
   ΔNWC, cash taxes → **free cash flow available for debt paydown**. Show the schedule.
4. **Debt schedule.** Beginning debt → interest (rate × balance) → mandatory amort +
   **cash sweep** (excess FCF pays down debt) → ending debt, per year. Watch the
   interest⇄FCF circularity (interest depends on balance, paydown depends on FCF).
5. **Exit.** Exit EV = exit EV/EBITDA × exit-year EBITDA (default exit multiple = entry,
   note expansion/compression). Exit equity = exit EV − net debt at exit.
6. **Returns.** On the sponsor's initial equity: **MOIC = exit equity / entry equity**;
   **IRR** over the hold (default 5 yr). Show a sensitivity grid: entry multiple ×
   exit multiple (and/or leverage) → IRR.

## Output
Report Builder: Sources & Uses table, operating model, debt schedule, returns summary
(IRR/MOIC), and the sensitivity grid — each via `report_add_component`. Only assumption
drivers (multiples, leverage, rate, growth/margin) and historical EBITDA/debt are hard
inputs; everything else is derived and shown. Present as analysis, not investment advice.
