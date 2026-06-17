# Analysis skills — attribution

The financial-analysis methodology in this directory (comps, DCF, earnings note)
is **adapted from** Anthropic's *Claude for Financial Services* reference
implementation:

  https://github.com/anthropics/financial-services
  (plugins/vertical-plugins/financial-analysis), licensed under **Apache-2.0**.

The text has been rewritten and condensed for OpenTerminal: it targets the app's
own read-only data tools (`edgar_*`, `get_quote`, `get_observations`) and the
in-app Report Builder instead of the upstream's Excel/openpyxl + commercial MCP
connectors (Daloopa/FactSet/Kensho). Methodology and structure derive from the
upstream skills; no upstream code or example files are included.

Apache-2.0 © Anthropic, with modifications © OpenTerminal.
