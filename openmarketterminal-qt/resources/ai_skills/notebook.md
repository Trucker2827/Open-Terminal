# Skill: Build a live analysis notebook

Use when the user wants an analysis they can **run and tweak themselves** (interactive
DCF, comps, fundamentals) rather than a static chat answer or a Report Builder memo.
Call **`notebook_create(title, cells)`** — it writes a Python notebook into the Notebooks
screen and opens it; the **user runs the cells**.

`cells` is an ordered list of `{type: "code"|"markdown", source: string}`.

Code cells run in the notebook kernel where **`edgar` (edgartools), `pandas`, `numpy`**
are available. Use the RELIABLE edgartools getters — the cash-flow getters
(`get_free_cash_flow`, `get_operating_cash_flow`) often return `None`:
```python
import os, pandas as pd
from edgar import Company, set_identity
set_identity(os.environ.get("EDGAR_IDENTITY", "OpenTerminal research@openterminal.app"))
fin = Company(TICKER).get_financials()
fin.get_revenue(); fin.get_net_income(); fin.get_operating_income()
fin.get_shares_outstanding_diluted(); fin.get_stockholders_equity()
fin.income_statement().to_dataframe()      # multi-year; columns include period strings
```

## How to structure it
1. **Markdown** cell: title + one line on what the notebook does.
2. **Code** cell: `TICKER = "AAPL"` (clearly editable) + imports + `set_identity` + fetch.
3. **Code** cells: compute and **display pandas DataFrames** (the kernel renders text/tables
   — avoid `matplotlib`, inline plots aren't supported). Short markdown between sections.
4. **Markdown** footer: "educational analysis, not investment advice."

Keep cells small and **runnable top-to-bottom with the default ticker**. For a DCF, expose
assumptions as editable parameter cells (`GROWTH = 0.08`, `WACC = 0.09`, `TERM_G = 0.025`,
and a `BASE_FCF = ...` the user can set, since cash-flow getters are unreliable) and show a
sensitivity DataFrame. The user edits the ticker/assumptions and re-runs.

Prefer this skill (interactive notebook) when the user says "let me play with it / a model
I can change"; prefer the Report Builder playbooks (comps/dcf/earnings) for a finished memo.
