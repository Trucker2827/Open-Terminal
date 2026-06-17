# Skill: DCF Valuation (discounted cash flow)

Use when the user asks for intrinsic value, a DCF, or "what's it worth on fundamentals."
Data via the app's tools only: `edgar_resolve_cik`, `edgar_get_financials`,
`edgar_calc_multiples` (for the exit-multiple cross-check), `get_quote`.

Confirm at each stage before building the next — a wrong margin assumption found after
the sensitivity table means rebuilding everything downstream.

## Method
1. **History (3–5 yrs).** `edgar_get_financials` → revenue, EBIT, margins, capex, D&A,
   working-capital trend, shares, debt, cash. Establish the margin progression and a
   base FCF margin. Show it; confirm.
2. **Revenue projections (5 yrs).** Apply explicit growth rates per year, decaying
   toward the terminal rate. Show $ and growth %. Benchmark the rates against history
   and (if comps were run) peer growth. Confirm before margins.
3. **FCF build (in sequence):** Revenue → EBIT (margin assumption) → taxes (effective
   rate) → NOPAT → + D&A → − capex → − ΔNWC = **unlevered FCF** per year. Show the
   schedule; confirm.
4. **WACC.** Cost of equity = rf + β·ERP (state assumptions, e.g. rf≈current 10y, ERP≈5%).
   After-tax cost of debt from interest/total-debt × (1−tax). Weight by market values.
   No debt → WACC = cost of equity. Show the calc; confirm. Typical range 7–11%.
5. **Terminal value.** Gordon growth: `TV = FCF_n·(1+g)/(WACC−g)` with g ≈ 2–3%
   (never ≥ WACC). Cross-check against an exit EV/EBITDA from `edgar_calc_multiples`
   /comps median. Discount all FCF + TV at `1/(1+WACC)^t`.
6. **Equity bridge.** Enterprise value − net debt = equity value ÷ shares =
   **implied price/share**. Compare to the live `get_quote` price → upside/downside.
7. **Sensitivity.** A WACC × terminal-g grid of implied price; the **center cell uses
   the base-case WACC and g and must equal the model's implied price** (sanity check).

## Output
Report Builder: assumptions block, FCF schedule table, WACC calc, valuation summary
(EV → equity → per share), and the sensitivity table — each via `report_add_component`.
Only raw historicals, assumption drivers, and current market data are "hard" inputs;
everything else is derived and shown transparently. State conclusions as analysis,
not investment advice.
