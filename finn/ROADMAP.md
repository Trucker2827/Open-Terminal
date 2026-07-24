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

## Pillar 2: Predictions

The crown jewels: the duel protocol, the arena, the calibrator, hourly
settlements as ground truth. "Better" means making Kalshi work feel like
operating an instrument, not reading a firehose:

- The Kalshi screen should answer, per contract, in seconds: how far is
  strike in *sigmas*, what does the engine think, what does the calibrator
  think, what do arena models think — each labeled with its measured
  trustworthiness (or lack of it).
- Settlement history is a first-class record: honest P&L per settlement
  (netting-aware), difficulty-cohorted hit rates, streaks — the user's own
  scoreboard, as rigorous as the models'.
- The arena and duel should be reachable from the Kalshi workflow, not
  hidden in Research Lab — they are context for every contract decision.
- Calibration everywhere: any probability shown near a contract carries its
  source and track record. An unmeasured probability is displayed as an
  opinion, not a number.
- Season/epoch mechanics stay preregistered and mechanical. No discretionary
  verdicts, ever.

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
