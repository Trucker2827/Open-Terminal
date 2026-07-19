# OpenTerminal Project Memory

## Purpose

OpenTerminal is a local-first trading, market-research, and automation terminal.
Its guiding architecture is:

```
market data -> feature store -> models and rules -> risk gates -> execution -> audit
```

The terminal must make it easy to inspect and operate the system without
pretending that a model has an edge before evidence supports that conclusion.

The CLI is the primary control plane. The GUI is the visual and interactive
surface for the same local profile, data, automation, and audit trail.

## Non-Negotiable Principles

- Profiles, credentials, models, journals, and analytics remain local to the
  user's machine unless the user directly configures a provider or broker.
- API keys, broker secrets, passwords, PINs, session tokens, and private keys
  are never written to source control, reports, logs, or this document.
- LLMs can inspect, summarize, propose, and operate through approved CLI
  commands. They do not replace deterministic risk, execution, and audit
  checks.
- The model path is separate from the order-authority path. A recommendation
  is not an order.
- Every decision must be traceable: observed data, timestamps, feature values,
  model or rule version, risk result, order attempt, fill, fee, and outcome.
- Paper and live activity are recorded separately. Live performance must never
  be inferred from paper results.
- Claims of profitability require cost-net, settled, independently clustered
  evidence. A high win rate alone is not enough.

## Runtime Topology

```
                    +---------------------+
                    |   OpenTerminal GUI  |
                    | profile / charts /  |
                    | cockpit / controls  |
                    +----------+----------+
                               |
                    attached local profile
                               |
 +----------------+  +---------v---------+  +----------------+
 | CLI headless   |  | local profile data |  | daemon service |
 | inspection and +->| journals / lake /  |<-+ collectors and |
 | one-shot jobs  |  | secure settings    |  | scheduled jobs |
 +----------------+  +---------+---------+  +----------------+
                               |
       +-----------------------+------------------------+
       |                       |                        |
 +-----v------+         +------v------+          +------v-------+
 | exchanges  |         | prediction  |          | AI providers |
 | Coinbase   |         | Kalshi       |          | Ollama local |
 | Kraken     |         | contracts    |          | OpenAI       |
 | Alpaca ... |         +-------------+          | Anthropic... |
 +------------+                                +--------------+
```

### Ownership Rule

One selected profile has one long-running live endpoint owner at a time:

- The GUI is the interactive owner when it is running normally.
- The daemon is the unattended owner when it is serving the profile.
- CLI attach mode talks to the existing owner.
- Headless CLI commands are one-shot operations and must not silently become a
  second long-running owner.

Use `sync status` to see the current ownership and attachment state before
starting another long-running process.

## Profile, Security, and Local AI

Each profile is local and owns its own:

- display name and optional contact fields;
- broker and market-provider configuration;
- AI provider and model selection;
- secure credentials held outside the repository;
- journals, daemon jobs, local data lake, and automation state.

Supported AI-provider concepts include OpenAI, Anthropic, and local Ollama.
Ollama is intended for a local model endpoint on the user's machine. Cloud
providers are contacted only when the user selects them and supplies direct
credentials.

PIN behavior is local as well. The user can configure a PIN and can enable or
disable the requirement to enter it at app launch. Disabling launch locking
does not expose the PIN value or remove manual security controls.

Useful commands:

```sh
./build/openterminalcli --profile default profile show
./build/openterminalcli --profile default settings list
./build/openterminalcli --profile default security network-audit
./build/openterminalcli --profile default ai providers
./build/openterminalcli --profile default ai test
```

## CLI: The Power Surface

The executable is `./build/openterminalcli`. Its safe orientation pattern is:

```sh
cd /Users/haydarevich/src/Open-Terminal/openmarketterminal-qt

./build/openterminalcli --profile default status
./build/openterminalcli --profile default doctor
./build/openterminalcli --profile default sync status
./build/openterminalcli --profile default serve --status
./build/openterminalcli --profile default daemon status
```

`--headless` uses the local core directly. Without it, commands that need the
interactive application can attach to the running GUI or local serve process.
Use `--json` whenever another program or an LLM needs structured output.

### Core CLI Areas

| Area | Examples | Purpose |
| --- | --- | --- |
| Health and ownership | `status`, `doctor`, `sync status`, `daemon health` | Identify the active profile, daemon, and runtime health. |
| Accounts and profiles | `profile`, `settings`, `trade accounts` | Configure local identity and connected venues. |
| Market data | `quote`, `coverage`, `news`, `research`, `macro` | Inspect fresh prices, data coverage, and research. |
| Crypto | `crypto ticker`, `crypto book`, `crypto fees`, `crypto readiness` | Inspect executable crypto conditions and estimated cost. |
| Kalshi | `kalshi auto status`, `kalshi auto positions`, `kalshi auto calibration` | Inspect prediction-market surface, evidence, and positions. |
| Edge and models | `edge context`, `edge journal`, `edge decision-cockpit`, `edge chronos2 forecast` | Inspect decisions, model inputs, and candidate outputs. |
| Daemon | `daemon jobs list`, `daemon collectors status`, `daemon monitors status` | Operate scheduled local work. |
| Data lake | `data lake status`, `data lake query`, `data lake mirror-decisions` | Inspect and analyze local evidence. |
| AI and agents | `ask`, `ai providers`, `ai recipes`, `ai run strategy`, `agent list` | Use configured AI with visible local controls. |
| Research artifacts | `notebook`, `report`, `notes`, `files`, `excel` | Create reproducible analysis and reports. |

Commands that can submit, cancel, arm, disarm, restart, remove, or change
configuration are operational commands, not diagnostic commands. An LLM must
display intent, venue, side, amount, limit, cost estimate, and risk status
before it invokes one.

## Daemon and Local Automation

The daemon is a profile-local service for persistent collection and scheduled
work. On macOS it can be installed as a per-user `launchd` LaunchAgent.

Daemon responsibilities include:

- running local collectors and health checks;
- recording crypto ticks, order-book snapshots, and market observations;
- running scheduled research, notebook, notification, and strategy jobs;
- producing decision and broker-event journals;
- maintaining data-lake mirrors;
- serving a local endpoint for attach-mode CLI and GUI coordination.

Daemon controls:

```sh
./build/openterminalcli --profile default daemon install
./build/openterminalcli --profile default daemon start
./build/openterminalcli --profile default daemon status
./build/openterminalcli --profile default daemon health
./build/openterminalcli --profile default daemon jobs list
./build/openterminalcli --profile default daemon collectors status
./build/openterminalcli --profile default daemon monitors status
```

The GUI should expose daemon state clearly: green means a healthy active local
daemon, red means unavailable or unhealthy, and a manual restart is an
operator action. Daemon status is a runtime fact and must always be verified
through `daemon status` or `serve --status`, not inferred from an old screen.

## Local Data and Evidence Lake

OpenTerminal persists local observations and decisions so that systems can be
measured rather than remembered selectively.

The data lake uses profile-local JSONL journals with a local DuckDB analytics
database under the profile's `data/lake` directory. It is designed to hold:

- raw spot ticks and order-book snapshots;
- Kalshi market snapshots, order books, trades, fills, positions, and orders;
- news and research summaries with timestamps and sources;
- model features, forecasts, calibration snapshots, and decision explanations;
- execution attempts, confirmed fills, fee data, and lifecycle events;
- resolved outcomes and P/L for paper and live activity.

Relevant commands:

```sh
./build/openterminalcli --profile default data lake status
./build/openterminalcli --profile default data lake manifest
./build/openterminalcli --profile default data lake mirror-edge
./build/openterminalcli --profile default data lake mirror-decisions
./build/openterminalcli --profile default data lake mirror-broker-events
```

Journals need rotation and bounded retention. A data collector that fills a
disk is a failure, not evidence.

## Crypto: Spot, Scalp, and Execution Cost

Crypto work is centered on Coinbase and Kraken market data and account-aware
execution, with other venues available only when the profile config supports
them. The Crypto window is the execution cockpit for spot and scalp workflows,
not the Equity screen.

The system records and displays:

- best bid, ask, spread, depth, imbalance, trades, and source freshness;
- independent spot references across configured venues;
- maker versus taker assumptions;
- fees, spread, slippage, safety buffer, and round-trip breakeven;
- live, paper, pending, filled, cancelled, and rejected order states;
- human versus automation origin where the source is known.

The maker-only crypto form exists to prevent an accidental marketable order:
the user enters an asset, cash amount, and limit price, and submits a
post-only order. It supports the selected supported crypto pair, not BTC only.

Useful read-only checks:

```sh
./build/openterminalcli --profile default crypto readiness
./build/openterminalcli --profile default crypto fees BTC/USD
./build/openterminalcli --profile default crypto ticker BTC/USD
./build/openterminalcli --profile default crypto book BTC/USD
./build/openterminalcli --profile default crypto balance
./build/openterminalcli --profile default crypto orders
./build/openterminalcli --profile default crypto fills
```

### Scalp and Spot Rules

Scalping is not just "buy low, sell high." It must clear:

```
expected move > entry fee + exit fee + spread + slippage + safety buffer
```

Small orders can be mathematically unworkable when venue fees or spreads are
large. Venue-specific cost profiles are required; Coinbase Advanced, Kraken
Pro, Alpaca crypto, and any future venue must not share a single global fee.

The daemon has scalp and strategy-related commands, including venue and maker
inspection. These are intended for paper-first, cost-net evidence collection:

```sh
./build/openterminalcli --profile default daemon scalp venues --maker
./build/openterminalcli --profile default daemon scalp status
./build/openterminalcli --profile default daemon scalp tape BTC-USD --limit 20
./build/openterminalcli --profile default edge scalp-gate BTC-USD
./build/openterminalcli --profile default edge spot-swing-gate BTC-USD
```

## Prediction Markets and Kalshi

Prediction-market work must distinguish two synchronized but different prices:

1. The underlying spot reference: BTC, ETH, SOL, or another asset price from
   independent configured market-data sources.
2. The Kalshi contract: executable YES and NO bid/ask, depth, spread, and the
   user's contract position.

The Predictions window is intended to be a settlement-race view rather than a
percentage-only chart. It should show:

- the settlement target and time remaining;
- independent spot price, distance to target, velocity, and source freshness;
- Kalshi YES and NO executable order book, spread, depth, and history;
- contract type such as binary, range, or "or above";
- open position, average cost, cash-out or reduce path, fees, and P/L;
- a concise, saved reason for each human or bot decision.

For an early exit, "cash out" is normally a reducing or opposite order before
settlement. It is not a guaranteed cash-out button: it needs an executable
counterparty and must preserve the real order-book price, spread, and fee.

### Kalshi Evidence and Automation

The Kalshi auto engine is a local evidence, calibration, and candidate system.
It includes surface construction, settlement-average probability modeling,
realized-volatility estimates, empirical checks, isotonic calibration,
attribution, and cohort significance reporting.

Core inspection commands:

```sh
./build/openterminalcli --profile default kalshi auto status
./build/openterminalcli --profile default kalshi auto opportunities
./build/openterminalcli --profile default kalshi auto audit
./build/openterminalcli --profile default kalshi auto calibration
./build/openterminalcli --profile default kalshi auto cohort-significance
./build/openterminalcli --profile default kalshi auto attribution
./build/openterminalcli --profile default kalshi auto events
./build/openterminalcli --profile default kalshi auto positions
```

The current codebase contains bounded-session and live-status concepts such as
`kalshi auto live status` and `kalshi auto live session`. Their presence is
not proof that unattended live order submission is active. In the inspected
command path, the base auto surface explicitly reports paper-only behavior and
structurally unavailable live submission. Always use the current runtime
status, account state, and execution response before treating any activity as
live.

Paper candidates should be abundant enough to measure selection logic. Live
activity, when intentionally enabled by a human, must remain bounded by the
current configured session, stake, order rate, exposure, daily loss, and kill
switch rules. Neither this document nor an LLM may bypass those controls.

## Strategy Sandbox and Honest Scorekeeping

The strategy sandbox is a paper-first proving ground. Its goal is to compare
strategy books without rewriting their history or hiding execution cost.

Design features:

- books are parameter-hashed so a changed model or parameter set starts a new
  evidence track;
- candidate producers feed journals rather than directly placing orders;
- paper fills model maker and taker behavior separately;
- resolver and scorer use real recorded prices where available;
- cost-net P/L includes fees, spread, slippage, and conservative fill logic;
- scorecards cluster evidence by independent session or settlement event;
- thin, degraded, unknown, or correlated data cannot earn authority;
- live eligibility is report-only evidence, not an automatic permission grant.

Every experiment should be evaluated as:

```
signal -> decision -> paper order -> fill or no fill -> exit or settlement -> cost-net result
```

Do not mix Kalshi contract P/L, spot P/L, scalp P/L, and hypothetical
long/short P/L into one number. They have different instruments, horizons,
costs, and failure modes.

## Forecasting and AI Strategy

### Chronos-2

Chronos-2 is a local time-series forecast candidate. It is used as a producer
of timestamped forecasts for crypto and equity horizons, not as direct order
authority. Forecasts are compared against simple baselines and market prices,
then must survive cost-net sandbox evidence before any promotion discussion.

Separate horizon models can use shared past and current context:

- short horizons: 5m and 15m;
- intermediate: 1h;
- longer horizon: 1d.

Cross-horizon context must be timestamped. No feature, settlement label, or
other model output produced after the decision timestamp may enter a training
or backtest row.

### LLM and Agent Role

Configured LLMs can use the CLI as a high-bandwidth operating interface. They
can inspect fresh market context, research, ledger state, risk configuration,
and candidate explanations. They can summarize a thesis and build a structured
trade plan.

The LLM must not be a magical probability source. It does not bypass:

- market and venue filters;
- freshness and data-quality checks;
- deterministic risk floors;
- per-strategy and aggregate exposure caps;
- configured human arming and global kill switch;
- broker confirmation and journal reconciliation.

The intended decision chain is:

```
data agents -> signal and model candidates -> deterministic risk engine
           -> approved execution path -> broker response -> audit journal
```

News and research are useful slower context for hourly and daily decisions.
They are less reliable as the sole input for a 15-minute contract or a
microstructure trade. Local microstructure, order books, and fresh prices
remain the primary short-horizon evidence.

## GUI Surface Map

| Screen | Role in the current system |
| --- | --- |
| Profile and Settings | Local identity, AI providers, accounts, security, PIN behavior, and automation controls. |
| Crypto | Coinbase/Kraken-oriented spot and scalp cockpit, DOM, order ticket, fee and cost views, positions, orders, fills, and history. |
| Predictions | Kalshi contract discovery, settlement race, contract order book, position lifecycle, P/L, maker order flow, and auto cockpit. |
| Strategies | Strategy evidence, proof books, experiments, calibration, deployments, scans, alerts, and research links. Old templates should never be presented as proof of a working edge. |
| Notebooks / Research Lab | Postmortems, replay, calibration, cohort analysis, strategy experiments, and portfolio research backed by local evidence. |
| AI Chat / Agent Studio | Model selection, tool run audit, agent work, and explanation. Agent output must identify model, local or cloud status, data sources, requested and executed tools, and results. |
| Dashboard | Compact operating status and selected widgets, not duplicated page titles or decorative noise. |

## Human, Bot, Paper, and Live Labels

Every order and P/L view should label origin and mode clearly:

- `HUMAN`: initiated by a user through GUI or CLI;
- `BOT`: created by a configured automation path;
- `PAPER`: simulated, never sent to a broker or venue;
- `LIVE`: submitted to a configured venue and verified from the response;
- `PENDING`: submitted or planned but not yet confirmed filled;
- `CLOSED`: resolved, exited, cancelled, or otherwise complete.

P/L views should sort newest activity first and separate open positions from
closed results. Each row needs a unique internal ID, a contract or symbol, a
side, a mode, fees, cost, realized or unrealized P/L, and a short persisted
reason for the decision.

## Operational Checklist

Before collecting or trading:

1. Confirm profile, selected venue, account, and mode.
2. Run `sync status`, `daemon status`, and the relevant venue readiness check.
3. Confirm data freshness, order-book quality, fees, and estimated total cost.
4. Confirm paper versus live mode and the human-controlled arming state.
5. Confirm kill-switch and exposure limits are known.
6. Use a bounded session or explicitly stop the daemon after the work.
7. Review journals, broker events, fills, and P/L after the session.

For Kalshi, additionally confirm contract rules, target, close time, contract
type, YES and NO executable quotes, and whether the current spot reference is
the official settlement source or only an independent visual reference.

## Known Limits and Honest Language

- A model probability is not a guarantee and does not make a contract
  executable at that price.
- A market can move faster than an LLM, GUI refresh, REST poll, or limit order
  fills. Latency and queue position matter.
- A maker order can avoid a taker fee but may not fill, or can fill when price
  has moved against it.
- Prediction-market early exits require liquidity. A displayed probability is
  not an available exit price.
- News, social content, and whale narratives are noisy features, never proof.
- Older or legacy strategy books can contain assumptions that are not honest
  enough for new evidence. Preserve history, but do not promote it blindly.
- Runtime and branch state changes quickly. Verify current behavior with CLI,
  tests, and the running application instead of relying solely on this file.

## Maintenance Rules for This File

Update this document when a change affects:

- live/paper authority or the human arming workflow;
- daemon ownership, jobs, collectors, or health behavior;
- CLI command contracts or high-value workflows;
- supported venues, data sources, or local storage;
- model inputs, calibration, or no-lookahead protections;
- GUI location of a meaningful operational control.

Never add secrets, account balances, order IDs, personal contact details, PINs,
or private endpoint credentials here. Link to code, local help, and test names
instead of copying volatile implementation detail into project memory.
