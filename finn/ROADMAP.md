# Open Terminal — Product Roadmap (what "better" means, per pillar)

This file aims the Finn loop. Scouts read it to find work that matters;
specs cite it so every issue ladders up to a direction. It is the operator's
product voice — edit it freely; the loop adapts.

The one-line thesis: **Open Terminal is the honest trading research
terminal — every number is real, every signal is measured, every model
earns trust before it is believed.**

## Pillar 1: Crypto

The user's real situation: ~$2k of capital, Coinbase Advanced at the 40bps
tier, a paper spot engine accumulating a scoreboard that gates any real
money. "Better" means shortening the distance from *signal* to *justified
decision*:

- The paper scoreboard's verdict should be impossible to miss and impossible
  to misread: what would this engine have earned net of fees, over what
  sample, with what confidence — in one glance.
- Fee reality everywhere: any displayed opportunity shows net-of-round-trip
  economics for the user's actual tier, never gross moves. (The 95–110bps
  hurdle is the house number.)
- The noise floor (move-in-sigmas) and ambient vol should be visible where
  decisions happen, not buried in CLI JSON.
- Cross-venue data (coinbase/kraken/gemini) is a strength — surface spread
  and lead/lag insight, not just per-venue ticks.
- Measured trust flows in: quant signals and IC verdicts belong beside the
  engine's own calls, labeled with their evidence.
- What does NOT belong: any autonomous order path, leverage anything,
  bigger-model-will-fix-it fantasies. The engine stays deterministic; models
  stay advisory.

## Pillar 2: Predictions — CURRENT FOCUS (operator, 2026-07-24)

The goal is now explicit: **automate Kalshi yes/no bids, runnable as a bot
and via CLI.** The human arms it; after that, the game is on — the bot
bids autonomously inside the charter's fence (caps, kill switch,
preregistered trust gate; see the charter's carve-out). The substrate is
merged: the gated live path (PR #39), honest live-order accounting
(PR #44), and a calibrator that currently beats the market baseline
(adds_value_over_market true at 228 resolved — re-check live, never
assume).

The build ladder, in order — each rung is only as good as its proof:

1. **Paper bot MVP**: `openterminalcli kalshi bot` consumes calibrator
   edges, decides yes/no + price + size, journals every decision with its
   reasoning, settles against real hourly settlements. Paper first, always.
2. **Preregistered promotion gate**: a sealed, mechanical rule (like an
   arena season) that decides when paper graduates to micro-live —
   sample floor, net-positive after fees, Brier vs market baseline,
   drawdown limit. No discretionary promotion, ever.
3. **GUI parity**: the Predictions/Kalshi window reflects EXACTLY what is
   implemented — bot status, armed state, caps in force, live decisions
   as they happen, the paper/live scoreboard, the promotion gate's
   current verdict. If the CLI can see it, the window shows it.
4. **Observable loop**: launchd job, status chip (green/amber/grey),
   kill-switch that the GUI and CLI can both throw.
5. **Micro-live behind the human arm**: `--mode live` refused unless the
   session is armed AND the promotion gate reads PASS. Then: game on.
6. **Order lifecycle honesty**: quote TTLs, cancel/replace, resting
   orders counted as risk (the #44 ledger is the single source of truth).

Still standing from before (context for every contract decision): sigmas
and calibrated probabilities with track records on the Kalshi screen,
settlement history as the user's own scoreboard, the arena reachable from
the workflow, no unmeasured probability displayed as a number.

## Pillar 3: Equity

The least-loved pillar; the standard is honesty and depth over breadth:

- EDGAR is the backbone (filings, 13F holders, insiders, XBRL financials) —
  brittle scraping retires wherever EDGAR or native services can serve.
- The research flow should chain: symbol → quote → filings → news →
  peers → notes/report, without dead ends between screens.
- The Quant Lab's honest tooling (train/IC/screen on local data) should meet
  equities: screens that carry their IC caveat, watchlists that can be fed
  from a model screen WITH its measured (usually humbling) predictive power
  attached.
- Data screens that cannot be real get cut, per the AKShare precedent. No
  decorative panels.

## Cross-cutting (applies to every pillar)

- Evidence files are the integration bus: features publish and consume
  through them; direct coupling between subsystems is a smell.
- Every loop (launchd) is observable: status, last-run, freshness visible
  somewhere a human looks.
- The installed app is the product. Features that only work in dev builds
  are unfinished (CLI bundling taught this).
- Windows and Linux users get the same honesty macOS gets.
