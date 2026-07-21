# Scripted advise open → commit-blind wrapper

Runs one firewalled advisory prediction inside the tight (~30s) window that the
advisory protocol imposes (`advise open` needs a ≤15s-fresh snapshot; crypto
strike `prediction_ttl` is ~30s). Opens a challenge, forecasts blind, and
commits — with a hard timing guard that **never commits a stale-window
prediction**. A prediction that arrives too late leaves the challenge to expire
honestly (recorded `EXPIRED` in the durable ledger).

## Usage

```bash
# Auto-pick the freshest openable contract, forecast with Claude, commit if in-window:
scripts/kalshi_advise/advise_challenge.py --auto

# Specific contract:
scripts/kalshi_advise/advise_challenge.py --ticker KXBTCD-26JUL2106-T66099.99

# Test the whole pipeline without committing (opens a real challenge, forecasts,
# reports timing, does NOT commit-blind — the challenge expires):
scripts/kalshi_advise/advise_challenge.py --auto --dry-run

# Plug in your own forecaster:
scripts/kalshi_advise/advise_challenge.py --ticker T --forecaster ./my_forecaster.py
```

Key flags: `--cli <path>` (openterminalcli, default `build/openterminalcli`),
`--profile`, `--safety-margin-ms` (default 6000 — abort if less than this much
TTL remains at commit time), `--dry-run`.

## Forecaster contract (pluggable)

The forecaster is any command implementing two modes:

- `forecaster identify` → stdout JSON
  `{"provider","model","prompt_version","temperature"}`
  (fast; must NOT call an API — the wrapper needs this before `advise open` to
  tag the challenge with the forecaster's identity).
- `forecaster predict` with the **blind context JSON on stdin** → stdout JSON
  `{"probability":0..1, "confidence":0..1, "rationale":"..."}`.

The blind context piped to `predict` is exactly what `advise open` emits — a
**price-free** packet (strike, spot, distance, required move, seconds-left,
settlement band/definition, horizon, realized move/vol, spot microstructure).
It contains no Kalshi quote, market-implied probability, daemon probability, or
contract flow. The wrapper re-verifies this (defense in depth) and aborts before
ever handing the forecaster a leaking context.

`provider`/`model`/`prompt_version` are the **frozen experimental unit** —
recorded on every challenge for attribution. Freeze them for a trial epoch;
change them only at an epoch boundary.

## Reference forecaster — `claude_forecaster.py`

Calls the Claude API with a frozen, price-blind prompt and structured output.
Defaults: `claude-opus-4-8`, `effort=low`, thinking off (for latency), prompt
version `kalshi-blind-v1`. Env overrides: `ANTHROPIC_MODEL`,
`KALSHI_FORECASTER_PROMPT_VER`, `KALSHI_FORECASTER_EFFORT`,
`KALSHI_FORECASTER_FAST` (`1` = Opus fast mode — the lever when the ~30s window
is tight; premium pricing). Auth resolves `ANTHROPIC_API_KEY` /
`ANTHROPIC_AUTH_TOKEN` / an `ant auth login` profile automatically.

## Scope

open → commit-blind only. The market-informed second estimate
(`reveal → commit-post`) is a separate step; a `--post` mode that reveals the
market and asks the forecaster for a revised probability is a natural follow-up.
