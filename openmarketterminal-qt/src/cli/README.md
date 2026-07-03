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
    serve [--status|--stop]
    daemon install|start|status|stop # macOS login LaunchAgent wrapper for serve
    daemon health | logs | audit      # daemon visibility and safety checks
    daemon jobs list|add|run|remove   # local scheduled automation jobs
    daemon monitors status|repair     # monitor/job supervisor view
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
    macro dbnomics search inflation   # DB.NOMICS provider/dataset/series data
    macro fred UNRATE                 # FRED series via local FRED credential
    macro calendar 2026-07-03         # global economic calendar
    macro cb boe bank_rate            # central-bank series
    macro gov providers               # government data providers
    macro gov treasury-summary 2026-07-03
    trade accounts | drafts | paper   # accounts, audited drafts, paper/live reads
    data connections | connectors     # local data-source connection manager
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
    strategy backtest "Golden Cross" --symbol AAPL --capital 100000
    coverage trading --gaps
    security network-audit --no-sockets
    daemon install --start
    daemon status
    daemon health
    daemon jobs add brief NVDA --every-sec 86400
    daemon jobs add notebook trading-sma-crossover-backtest --every-sec 604800
    daemon paper meanrev --symbols AAPL,MSFT --every-sec 300 --max-iters 1
    daemon ai radar "AI semiconductors" --every-sec 3600
    daemon notify --title "OpenTerminal" --message "Daemon is alive"
    daemon stop
    daemon uninstall
    trade prepare buy AAPL 1 --type limit --limit-price 250 --reason "breakout setup"
    trade drafts --status prepared
    trade submit <draft-id> --mode paper --yes
    trade audit --limit 25
    ask <prompt...>
    ai providers                    # list local AI provider config
    ai use <provider>               # select/save openai, anthropic, or ollama
    ai test [prompt...]             # live test of active provider
    brief <target>
    risk <portfolio-or-symbol>
    thesis <ticker>
    radar <watchlist-or-topic>
    agent list | discover | run       # local agent registry and execution
    workflow list | show | save       # saved workflow CRUD and execution
    ai run strategy <meanrev|claude> --mode paper

## Exit codes
    0 ok · 2 usage · 3 no running instance · 4 token rejected
    5 tool/LLM returned success:false · 6 transport/HTTP error · 7 headless init error

## Notes
- Attach mode requires the GUI or `openterminalcli serve` running with a bridge.
- `--headless` initializes the core runtime directly and needs no GUI.
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
- `trade prepare` / `trade submit` use the audited two-phase order flow:
  prepare validates and records a local draft, submit re-runs risk checks and
  then routes through the existing guarded order-flow tool. Live submit requires
  `--yes --live-armed` at the CLI layer and still requires the GUI-owned live
  trading gates to be armed.
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
  strategy loop, and `deploy` opens the guarded GUI deployment flow.
- `research insiders`, `research 13f`, and `research 13f-top` use the same
  local SEC EDGAR tooling as the ownership screens. `research politicians` uses
  the local `ainvest_data.py` path and reads `AINVEST_API_KEY` from this machine;
  the CLI does not transmit or persist that key.
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
