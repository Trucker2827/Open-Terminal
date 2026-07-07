# openterminalcli

Power-user CLI for OpenTerminal. It can attach to a running GUI/daemon over the
localhost bridge, or run the core tool tree in-process with `--headless`.

## Usage
    openterminalcli [--json] [--headless] [--profile <name>] <command> [args]

    status                          # is an instance attached?
    version
    doctor [--live-ai]              # diagnose local CLI/profile setup
    security network-audit           # audit local MCP/AI/network exposure
    setup [status|profile|ai|doctor]# local profile/account + AI onboarding
    demo trading-system              # one-screen daemon/data/broker/gate proof
    sync status                      # GUI/daemon/CLI ownership alignment
    serve [--status|--stop]
    daemon install|start|status|stop # macOS login LaunchAgent wrapper for serve
    daemon owner|takeover|release     # warm daemon + ownership handoff
    daemon health | readiness | logs  # daemon visibility and trade-gate safety
    daemon jobs list|add|run|remove   # local scheduled automation jobs
    daemon monitors status|repair     # monitor/job supervisor view
    daemon collectors status|repair   # keep ticks and prediction snapshots warm
    screens [query]                  # list openable app screens
    open <screen-or-alias>            # open a GUI screen, e.g. profile, ai, markets
    cmd <command-bar text...>         # run raw command-bar navigation text
    actions [query]                   # list/search registered app actions
    action <id> [key=value ...]       # invoke a registered app action
    profile show                      # view local profile/account fields
    profile set name=... email=...    # edit local profile/account fields
    profile clear <field|all>         # clear local profile/account fields
    settings list [category]          # inspect local settings
    settings get <key>                # read one local setting
    settings set key=value category=x # write local setting(s)
    settings clear <key>              # remove local setting(s)
    mcp list | search <query> | describe <tool> | call <tool> '<json>'
    mcp call <tool> key=value ...
    tools [query]                    # alias: list or search tools
    tool <name>                      # alias: mcp describe
    call <tool> ...                  # alias: mcp call
    hub topics | peek <topic> | request <topic>
    quote <SYM...>                   # alias: q
    coverage [query] [--gaps]         # CLI parity by app area
    news latest | search <query>      # market news, monitors, clusters, threats
    research quote|company <SYM>      # equity research, filings, metrics
    research insiders AAPL --limit 20 # SEC Form 4 insider transactions
    research 13f-top BRK-B --limit 25 # SEC 13F holdings/top holders
    research politicians AAPL         # local AInvest-backed congress trades
    research health --gaps            # research source provenance/runtime gaps
    macro dbnomics search inflation   # DB.NOMICS provider/dataset/series data
    macro fred UNRATE                 # FRED series via local FRED credential
    macro calendar 2026-07-03         # global economic calendar
    macro cb boe bank_rate            # central-bank series
    macro gov providers               # government data providers
    macro gov treasury-summary 2026-07-03
    trade accounts | drafts | paper   # accounts, audited drafts, paper/live reads
    crypto balance | orders | fills   # Coinbase/crypto account reads
    crypto fees | readiness           # local fee profile and execution readiness
    crypto ticker BTC/USD             # crypto quote through configured venue
    crypto book BTC/USD --limit 5     # live order book
    crypto buy BTC/USD 0.0001 --limit-price 62000 --post-only
    data connections | connectors     # local data-source connection manager
    data lake status | kalshi | decisions | broker-events
    data lake mirror-edge --symbol BTC# mirror edge ticks/model outputs to lake
    data lake mirror-decisions       # mirror edge decisions to lake
    data lake mirror-broker-events   # mirror broker/order audit events to lake
    files list | read | write         # local File Manager artifacts
    notes list | create | show        # local research notes
    notebook list | open | run        # local notebook catalog + runner
    report state | add | save         # Report Builder documents
    excel read ./model.xlsx           # local CSV/XLSX spreadsheet access
    workspace layouts | open profile  # saved layouts + GUI panel control
    ai recipes | recipe run dcf AAPL  # bundled AI analysis playbooks
    notify providers | send | test    # local notification providers/triggers
    scanner list | add | events       # persisted scanner alert watches
    watchlist list                   # alias: wl
    watchlist show [id-or-name]
    watchlist create <name...> [--description TEXT] [--color HEX]
    watchlist delete <id-or-name>
    watchlist add <SYM...> [--watchlist ID]
    watchlist remove <SYM...> [--watchlist ID]
    watchlist lookup <company-or-symbol...>
    portfolio list                  # alias: port
    portfolio show [id-or-name]
    portfolio create <name...> [--owner NAME] [--currency USD]
    portfolio add <portfolio> <symbol> <quantity> <price>
    portfolio sell <portfolio> <symbol> <quantity> <price>
    portfolio tx <portfolio> [--limit N]
    playlist templates | follow | status
    playlist follow btc-hourly-edge --every-sec 60 --yes
    playlist run btc-hourly-edge
    strategy list | templates | show  # saved trading strategies
    strategy backtest <strategy> --symbol AAPL
    strategy paper-run meanrev --symbols AAPL
    files import <path> --yes
    files write <name> --content-file <path> --yes
    files download <id> <path> --yes
    files delete <id> --yes
    notes create <title...> --content-file <path> --yes
    notes update <id> priority=HIGH sentiment=BULLISH --yes
    notes export <id> --managed --yes
    report add heading --content "Investment Thesis" --yes
    report bulk @components.json --yes
    report metadata title="Daily Brief" author="Desk" --yes
    report save ./brief.openmarketterminal --managed --yes
    report load ./brief.openmarketterminal --yes
    notebook list --category Trading
    notebook path trading-sma-crossover-backtest
    notebook create "Pairs Research" --code "print('hello')" --no-open
    notebook run trading-sma-crossover-backtest --cell 1
    excel read ./model.xlsx --sheet Sheet1 --range A1:D20
    excel write ./model.xlsx '[["Symbol","Weight"],["AAPL",0.25]]' --managed --yes
    excel append ./model.xlsx @rows.json --sheet Sheet1 --yes
    workspace layouts --recent --limit 10
    workspace export "Equity Trader" ./equity-trader.layout.json --yes
    workspace template equity_trader --yes
    workspace open profile --exclusive
    workspace add dashboard news
    ai recipes
    ai recipe show dcf
    ai recipe run comps NVDA
    notify providers
    notify set webhook url=https://example.com/hook method=POST enabled=true --yes
    notify triggers news=true price=false --yes
    notify test webhook --yes
    notify send --provider webhook --title "Alert" --message "Check NVDA" --level alert --yes
    scanner add price AAPL above 250 --notify --yes
    scanner pause <watch-id> --yes
    scanner events --limit 50
    strategy templates
    playlist templates
    playlist follow btc-hourly-edge --every-sec 60 --yes
    playlist status
    playlist run btc-hourly-edge
    strategy backtest "Golden Cross" --symbol AAPL --capital 100000
    coverage trading --gaps
    security network-audit --no-sockets
    daemon install --start
    daemon owner
    daemon takeover --force --yes
    daemon status
    daemon health
    daemon readiness --symbol BTC
    daemon scalp venues --maker
    daemon scalp start BTC-USD --cadence-ms 250 --amounts 25,50 --venue coinbase_tier2 --liquidity maker --min-profit-bps 10 --paper
    daemon scalp status
    daemon scalp tape BTC-USD --limit 20
    daemon chronos2 BTC-USD --horizon 15m --every-sec 900 --timeout-sec 300
    daemon collectors repair
    daemon collectors status
    daemon collectors run       # one-shot: ticks, scalp gates, scoring, lake mirrors
    edge scalp-gate BTC-USD --venue coinbase --horizon-sec 15
    edge scalp-gate BTC-USD --watch --iterations 20 --interval-sec 5 --allow-warmup
    edge spot-swing-gate --symbols BTC,ETH,SOL --horizon 1h --venue coinbase_tier2
    edge crypto-recommend BTC/USD --venue coinbase --horizon-sec 60
    edge crypto-universe --symbols BTC,ETH,SOL --venue coinbase --horizon-sec 60
    edge decision-cockpit --symbols BTC,ETH,SOL
    edge chronos2 forecast BTC-USD --horizon 15m --publish
    edge chronos2 forecast BTC-USD --horizon 15m --journal --min-journal-edge-bps 15
    edge context BTC-USD
    edge context AAPL --asset-class equity
    edge journal score-crypto --symbol BTC-USD
    edge journal crypto-stats --horizon 60s
    edge journal evidence latest
    edge journal paper-sim --horizon 60s --amount-usd 100
    edge journal proof-loop --horizon 60s --amount-usd 100
    edge journal trust --horizon 60s
    edge journal regimes --horizon 60s
    edge journal no-trade --limit 20
    edge journal rare-alerts --min-edge-bps 50 --min-confidence 45
    edge journal replay --horizon 60s --amount-usd 100
    demo trading-system --symbol BTC --crypto-symbol BTC/USD --horizon 1h --market-prob 50
    daemon jobs add brief NVDA --every-sec 86400
    daemon jobs add notebook trading-sma-crossover-backtest --every-sec 604800
    daemon paper meanrev --symbols AAPL,MSFT --every-sec 300 --max-iters 1
    daemon ai radar "AI semiconductors" --every-sec 3600
    daemon notify --title "OpenTerminal" --message "Daemon is alive"
    daemon release
    daemon uninstall
    trade prepare buy AAPL 1 --type limit --limit-price 250 --reason "breakout setup"
    trade drafts --status prepared
    trade submit <draft-id> --mode paper --yes
    trade audit --limit 25
    crypto balance
    crypto orders BTC/USD
    crypto fills BTC/USD --limit 25
    crypto buy BTC/USD 0.0001 --limit-price 62000 --post-only
    crypto buy BTC/USD 0.0001 --limit-price 62000 --post-only --yes
    ask <prompt...>
    ai providers                    # list local AI provider config
    ai use <provider>               # select/save openai, anthropic, or ollama
    ai test [prompt...]             # live test of active provider
    brief <target>
    risk <portfolio-or-symbol>
    thesis <ticker>
    radar <watchlist-or-topic>
    edge microstructure BTC-USD --watch # live BTC tape + bid/ask pressure
    agent list | discover | run       # local agent registry and execution
    workflow list | show | save       # saved workflow CRUD and execution
    ai run strategy <meanrev|claude> --mode paper

## Exit codes
    0 ok · 2 usage · 3 no running instance · 4 token rejected
    5 tool/LLM returned success:false · 6 transport/HTTP error · 7 headless init error

## Notes
- Attach mode requires the GUI or `openterminalcli serve` running with a bridge.
- `--headless` initializes the core runtime directly and needs no GUI.
- `sync status` is the single-owner truth view. The selected local profile has
  one live endpoint owner at a time: the GUI for interactive work, or the daemon
  for unattended jobs. The CLI can attach to that owner or run one-shot headless
  commands, but it does not become a second long-running owner.
- `daemon` installs/manages a per-user macOS LaunchAgent that runs
  `openterminalcli --profile <profile> serve` at login. It uses the selected
  local profile, writes logs under that profile, and keeps live trading gated.
  The daemon also has a local JSON job registry for scheduled briefs, notebook
  runs, notification sends, health checks, and paper strategy loops. Jobs run as
  ordinary CLI child processes against the same profile, so permissions stay
  consistent and API keys remain local.
- `open`, `cmd`, `actions`, and `action` are GUI-control commands. Use them
  against the running app, not with `--headless`.
- `profile` / `account` commands run headless and edit only the local settings
  database for the selected `--profile`.
- `security network-audit` reads local MCP server config, AI provider endpoints,
  bridge metadata, AppConfig cloud placeholders, and live OpenTerminal sockets.
  Secrets are masked; MCP env values are reported only by key name. Use
  `--no-sockets` for a database/config-only report.
- `settings` commands are the low-level local settings escape hatch. Do not use
  them for secrets; provider/API keys belong in secure-storage-backed commands
  such as `ai use --api-key-env`.
- `watchlist` CRUD commands edit the selected local profile database directly;
  `watchlist lookup` uses the shared symbol lookup tool for ticker resolution.
- `portfolio` commands edit the selected local profile database directly and
  record BUY/SELL transactions when positions are added or sold.
- `playlist` commands are the strategy-playlist control plane: curated bundles
  of goal, risk profile, watched assets, CLI run command, daemon monitor, AI
  explanation role, and broker-execution boundary. `playlist follow ... --yes`
  stores the bundle under the local profile and, unless `--no-daemon` is used,
  creates the matching daemon job. Live trading is not automatic; playlists
  monitor, explain, paper-run, or prepare candidates until a separate guarded
  broker/prediction flow explicitly submits an order.
- `daemon owner` shows the current single-owner profile endpoint. `daemon start`
  starts the scheduler as bridge owner when the profile is idle, or as a warm
  background daemon when the GUI owns the bridge. If the GUI exits, the warm
  daemon promotes itself to bridge owner. Use `daemon takeover --force --yes`
  only when you deliberately want the CLI daemon to terminate the GUI owner and
  become bridge owner immediately. `daemon release` stops the CLI daemon while
  leaving the GUI owner alone.
- `trade prepare` / `trade submit` use the audited two-phase order flow:
  prepare validates and records a local draft, submit re-runs risk checks and
  then routes through the existing guarded order-flow tool. Live submit requires
  `--yes --live-armed` at the CLI layer and still requires the GUI-owned live
  trading gates to be armed.
- `crypto` is the direct CLI surface for configured crypto venues such as
  Coinbase. `crypto balance`, `orders`, `fills`, `ticker`, `book`, and
  `candles` are read paths. `crypto fees tiers` prints the Coinbase Advanced
  maker/taker schedule, and `crypto fees` shows or updates the local fee profile
  used by order previews, including maker/taker bps, rebates, free-fee allowance
  handling, and slippage bps. `crypto readiness` checks daemon reachability,
  bid/ask reads, balance reads, safety gates, allowed venues, and fee profile.
  `crypto buy|sell|order` prints a local cost preview by default, including
  gross fee, rebate/free adjustment, net fee, spread cost, slippage estimate,
  and round-trip breakeven. Add `--yes` only when you intend to submit; the order
  still routes through the existing gated `crypto_submit_order` tool.
  `crypto cancel` also requires `--yes`. Prefer `--post-only`/`--maker` limit
  orders when you want the command to avoid taker execution.
- `data` commands manage per-profile data-source connections. Connector
  definitions are available headless, so discovery works without opening the UI.
- `workflow` CRUD edits the selected profile database directly. `workflow run`
  and agent execution still go through MCP safety gates.
- `files` commands manage File Manager artifacts under the selected profile.
  Mutations require `--yes`; content can come from `--content-file` to avoid
  large shell arguments.
- `notes` commands manage the selected profile's research notes. `notes export
  --managed` writes a Markdown copy into File Manager.
- `notebook` commands expose the bundled notebook library from the CLI. Catalog
  notebooks are copied into the selected local profile before editing, and
  `notebook run` executes code cells through the local notebook kernel.
- `report` commands manage the Report Builder document model from the CLI.
  Mutations require `--yes` and persist between invocations via the current
  report file or report autosave. `report save --managed` imports the native
  report file into File Manager. PDF export still requires the GUI Report
  Builder screen.
- `excel` commands read and mutate local CSV/XLSX files via the same bundled
  spreadsheet helper used by workflow nodes. Mutations require `--yes`; use
  `--managed` after local writes/appends to import the output into File Manager.
  Google Sheets read/write helpers are available as `excel google-read`,
  `excel google-write`, `excel google-append`, and `excel google-clear`.
- `workspace` commands cover saved layouts headlessly (`layouts`, `show`,
  `export`, `import`, `delete`, `rename`, `templates`, `template`) and visible
  panel control through the running GUI (`panels`, `open`, `tab`, `add`,
  `replace`, `apply`). Layout mutations require `--yes`.
- `ai recipes` lists bundled analysis playbooks from `resources/ai_skills`.
  `ai recipe show <name>` prints the methodology, and `ai recipe run <name>
  <target>` sends a playbook-specific request through the configured local AI
  provider. API keys remain in local secure storage, same as `ai test`/`ask`.
- `notify` commands configure local notification providers and trigger toggles
  for the selected profile. Secret fields are masked in output. `notify test`
  and `notify send` wait for provider callbacks and return a non-zero exit code
  on failed delivery or timeout.
- `scanner` commands edit persisted scanner watches and event history for the
  selected profile. The one-shot CLI manages configuration; active watch polling
  is still owned by the app/runtime scanner monitor.
- `strategy` commands expose saved Strategy Builder rows. Backtests run through
  the native C++ backtest service; `paper-run` wraps the existing paper-only AI
  strategy loop, `playlist` routes into the strategy-playlist control plane,
  and `deploy` opens the guarded GUI deployment flow.
- `research insiders`, `research 13f`, and `research 13f-top` use the same
  local SEC EDGAR tooling as the ownership screens. `research politicians` uses
  the local `ainvest_data.py` path and reads `AINVEST_API_KEY` from this machine;
  the CLI does not transmit or persist that key.
- `edge context <symbol>` is the bridge between Dashboard/Equity history and edge
  decisions. It inventories local news, decision-journal rows, model-output
  snapshots, broker audit rows, and the public evidence commands that can be
  legally checked before a trade.
- `edge scalp-gate` is the strict intraday/scalping pre-trade gate. It estimates
  round-trip breakeven from fee, spread, slippage, and safety buffer, then blocks
  weak setups unless live tape, captured-move estimate, and local proof history
  clear the thresholds. Use `--watch --iterations N` for a paper observation
  session. It does not place orders. `daemon collectors repair` installs managed
  BTC/ETH/SOL scalp-gate jobs plus 15-second proof/trust scorecards, so the same
  gate can keep collecting evidence while the GUI is closed or only warming.
- `edge spot-swing-gate` is the longer-horizon companion to the scalp gate. It
  scans major spot crypto for 1h/4h/1d move candidates using round-trip fees,
  slippage, a safety buffer, and a larger minimum profit hurdle before it calls
  anything `BUY CANDIDATE`. It journals into the same `edge crypto-recommend`
  lane, so the decision cockpit and scoreboards can compare it with the rest of
  the system.
- `daemon scalp start` turns on the persistent paper-only millisecond
  microstructure engine inside the daemon. It keeps websocket/tick feeds open,
  builds 250ms/500ms/1s/5s rolling features, writes a local tick tape and paper
  decision journal, and refuses live execution. `daemon scalp venues` compares
  maker/taker cost floors by venue profile (`coinbase_advanced` base tier,
  `coinbase_tier2`...`coinbase_tier9`, `kraken_pro`, `binanceus`,
  `alpaca_crypto`) before you choose what to paper-test.
  `bitcointicker` is used only for BTC/BTC-USD and is automatically excluded
  from ETH/SOL/other symbols.
- `macro` is the friendly front door for economics data: FRED, BLS, calendar,
  central-bank series, ONS, StatCan, Census, DB.NOMICS, and government-data
  tools. Use `macro econ-run ...` or `macro tool <mcp-tool> ...` when a
  source-specific macro tool does not yet have a dedicated alias.
- `agent run`, `agent stock`, `agent team`, and `workflow run` require `--yes`;
  the MCP authorization layer may still deny operations that are not armed in
  local settings.
- stdout = data; stderr = diagnostics. Use `--json … | jq`.
- `mcp call` accepts JSON, `@file`, stdin via `-`, or shell-friendly `key=value`
  args.
- `ai use` stores cloud API keys in local secure storage. Prefer
  `--api-key-env ENV` or `--api-key-stdin` over passing keys directly on the
  command line.
- The CLI never sends the destructive token, and bridge.json carries no
  destructive token; live/destructive actions in attach mode are intentionally
  gated.

## Manpage and completions

Packaged CLI help lives under `packaging/cli`:

```bash
cmake --install build --component cli --prefix /usr/local

man ./packaging/cli/openterminalcli.1

# zsh
mkdir -p ~/.zsh/completions
cp packaging/cli/completions/_openterminalcli ~/.zsh/completions/
# add to ~/.zshrc if needed:
# fpath=(~/.zsh/completions $fpath); autoload -Uz compinit; compinit

# bash
source packaging/cli/completions/openterminalcli.bash

# fish
mkdir -p ~/.config/fish/completions
cp packaging/cli/completions/openterminalcli.fish ~/.config/fish/completions/
```
