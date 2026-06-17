# Skill: 3-Statement Model

Use when the user asks for an integrated financial model, a 3-statement build, or
projected Income Statement / Balance Sheet / Cash Flow. Data via the app's tools only:
`edgar_resolve_cik`, `edgar_get_financials` (historical IS/BS lines, shares, debt, cash),
`edgar_10k_sections` (drivers, segment detail). Build IS → BS → CF and **confirm the
checks at each stage** before moving on.

## Method
1. **Historicals (3–5 yr).** Pull revenue, margins, EBIT, net income, capex, D&A,
   working-capital lines, debt, cash, equity from `edgar_get_financials`. These are the
   only hard inputs besides assumption drivers.
2. **Assumptions.** State drivers: revenue growth, gross/EBIT margins, tax rate, capex %
   of revenue, D&A, NWC days, interest rate, dividend/buyback policy.
3. **Income Statement.** Revenue → COGS/gross → opex → EBIT → interest (from the debt
   schedule) → taxes → **net income**. Show margins each year.
4. **Balance Sheet (roll-forwards).** PP&E = prior + capex − D&A; debt = prior ± financing;
   **retained earnings = prior + net income − dividends**; working-capital items off the
   NWC drivers. **Check: Assets = Liabilities + Equity for every period** — show it.
5. **Cash Flow.** CFO (NI + D&A − ΔNWC) + CFI (−capex) + CFF (debt draws/repay, dividends,
   buybacks) = Δcash. **Check: CF ending cash = BS cash for every period** — show it.
6. **Integration & credit.** A revolver/min-cash plug balances the model. Report core
   credit metrics (Debt/EBITDA, interest coverage, FCF) and flag any covenant breach.
   Optionally Base / Upside / Downside scenarios off the driver set.

## Output
Report Builder: an Assumptions block and the three linked statements as tables (via
`report_add_component` `config={'csv':...}`), plus the **balance check** and **cash
tie-out** rows (both must hold), and a credit-metrics summary. Every projection is
derived from drivers + historicals, shown transparently. Present as analysis of the
model's mechanics, not investment advice.
