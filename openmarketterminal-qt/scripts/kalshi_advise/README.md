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

## App-LLM forecaster — `cli_forecaster.py` (no external key)

Uses the app's OWN configured LLM via `openterminalcli ai test`, which runs a
**pure completion with tools disabled** — so the model sees only the price-free
prompt and cannot look up the market (blind firewall holds). No
`ANTHROPIC_API_KEY` and no `anthropic` SDK required; it forecasts with whatever
provider the app is set to (e.g. a local Ollama model). This is the recommended
default when the app already has an LLM configured.

Trade-off is latency. A local model can be slow relative to the tight crypto
TTLs. Measured example: `llama3.3:latest` ≈ 23 s per forecast — fine for
contracts with a 45–60 s TTL (> 15 min to settlement), too slow for the 15–30 s
buckets. Keep it inside the window by constraining the picker to longer-dated
contracts:

```bash
advise_challenge.py --auto --forecaster ./cli_forecaster.py --auto-min-secs-left 900
```

…or point the app at a faster model (`openterminalcli ai use <provider>`).

## Scope

open → commit-blind only. The market-informed second estimate
(`reveal → commit-post`) is a separate step; a `--post` mode that reveals the
market and asks the forecaster for a revised probability is a natural follow-up.

## Unattended shadow advisor loop

`advisor_loop.py` runs the same firewalled challenge protocol continuously and
can never submit an order. It adds explicit abstention, deterministic cost-net
order proposals, a hash-chained append-only opportunity journal, atomic
restart state, and a frozen qualification policy. The proposal schema and gate
result are the interface a future canary adapter must consume; today the only
adapter is `ShadowExecutionAdapter`, which always records `submitted:false`.

```bash
# One opportunity (useful for smoke testing)
scripts/kalshi_advise/advisor_loop.py once --profile default \
  --forecaster scripts/kalshi_advise/cli_forecaster.py

# Persistent unattended process
scripts/kalshi_advise/advisor_loop.py start --profile default \
  --forecaster scripts/kalshi_advise/cli_forecaster.py --interval-seconds 60

scripts/kalshi_advise/advisor_loop.py status --profile default
scripts/kalshi_advise/advisor_loop.py report --profile default
scripts/kalshi_advise/advisor_loop.py stop --profile default
```

For a supervised Codex epoch, use `codex_forecaster.py` and install the
LaunchAgent. It is `RunAtLoad` and restarts only after abnormal exit; five
consecutive loop failures transition the process to `PAUSED_ERROR` instead of
creating a crash loop:

The Codex v4 firewall is structural, not prompt-based. Each prediction runs
ephemerally in a new empty temporary workspace with user configuration and
rules ignored. Shell, unified execution, code-mode, apps, browser, computer-use,
and image tools are disabled; the remaining read-only sandbox cannot read an
absolute path outside that empty workspace. A direct probe against the live
Kalshi book must return that it cannot access the file. Its frozen identity is
`kalshi-blind-codex-v4-zero-capability-latency-neutral`; earlier rows are separate, ineligible epochs.

```bash
advisor_loop.py install --profile default --forecaster ./codex_forecaster.py
advisor_loop.py uninstall --profile default
```

Persistent safety, promotion, and canary controls are separate from the LLM:

```bash
advisor_loop.py safety-observe --reconciled --open-exposure 0
advisor_loop.py evaluate
advisor_loop.py canary-configure --max-order-dollars 2 \
  --max-open-exposure 5 --daily-loss-limit 5
advisor_loop.py canary-enable       # rejected unless qualified + safe
advisor_loop.py canary-pulse        # rechecks everything, then uses execute-next
advisor_loop.py canary-disable
```

`canary-pulse` is the only bridge to the pre-existing deterministic live path.
It cannot run unless configuration is present, the frozen qualification report
passes, promotion state is `CANARY_ENABLED`, reconciliation is current, no
submission is unknown, and daily-loss/drawdown/consecutive-loss/exposure gates
are clear. Any later failed qualification or safety evaluation automatically
disables the canary.

Daily loss and unresolved-accounting blockers remain whole-account conservative.
Drawdown and consecutive-loss replay are scoped to `epoch_started_at_ms` written
by `canary-configure`, so historical discretionary losses cannot make a fresh
canary mathematically impossible. Reconfiguring deliberately starts a new
canary safety epoch and leaves the immutable advisory/settlement history intact.

State lives below the profile's `daemon/` directory. `advisor_opportunities.jsonl`
is hash chained: later edits or deletions are reported by `journal_valid:false`.
Qualification is fail-closed and versioned as `kalshi-qualification-v1`; it
requires at least 200 resolved rows, 80% paired daemon coverage, positive
improvement over market and daemon, a positive lower confidence bound, and
positive value after fees. Passing this report does not arm live trading.
