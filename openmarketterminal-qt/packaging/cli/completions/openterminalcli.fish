# fish completion for openterminalcli

set -l commands help status version doctor diagnose security sec setup onboard onboarding serve daemon service launchd screens open cmd actions action coverage profile settings mcp tools tool call quote q crypto-latency latency feed-race feedrace news research macro econ economics dbnomics govdata government trade trading data datasources ds agent agents workflow workflows wf files file notes note notebook notebooks nb report reports excel xl spreadsheet spreadsheets workspace layout layouts notify notification notifications scanner scan alerts watchlist wl portfolio port edge edge-radar edge_radar strategy strategies strat hub observe ask brief risk thesis radar ai

complete -c openterminalcli -f
complete -c openterminalcli -l json -d 'Print compact JSON'
complete -c openterminalcli -l headless -d 'Run in-process without GUI'
complete -c openterminalcli -l profile -x -d 'Use local profile'
complete -c openterminalcli -l help -d 'Show help'

for cmd in $commands
    complete -c openterminalcli -n "not __fish_seen_subcommand_from $commands" -a $cmd
end

complete -c openterminalcli -n "__fish_seen_subcommand_from mcp tools" -a "list search describe call"
complete -c openterminalcli -n "__fish_seen_subcommand_from security sec" -a "network-audit audit net --no-sockets"
complete -c openterminalcli -n "__fish_seen_subcommand_from ai" -a "providers use test ask brief risk thesis radar recipes recipe run strategy"
complete -c openterminalcli -n "__fish_seen_subcommand_from news" -a "latest search status monitors refresh mark-read save unsave"
complete -c openterminalcli -n "__fish_seen_subcommand_from research" -a "search quote company financials filings metrics insiders insider-summary 13f 13f-top politicians gov tool"
complete -c openterminalcli -n "__fish_seen_subcommand_from macro econ economics dbnomics govdata government" -a "fred bls calendar cb ons statcan census dbnomics gov econ-run run-econ tool providers datasets series observations search"
complete -c openterminalcli -n "__fish_seen_subcommand_from trade trading" -a "accounts drafts prepare submit cancel-draft paper live fast audit"
complete -c openterminalcli -n "__fish_seen_subcommand_from data datasources ds" -a "connectors connections get create update delete stats fields test tool"
complete -c openterminalcli -n "__fish_seen_subcommand_from files file" -a "list search info read path import write download delete stats storage"
complete -c openterminalcli -n "__fish_seen_subcommand_from notes note" -a "list search show create update favorite archive delete export"
complete -c openterminalcli -n "__fish_seen_subcommand_from notebook notebooks nb" -a "list ls show info path seed open create new run --category --difficulty --query --force --markdown --code --from-file --open --no-open --cell"
complete -c openterminalcli -n "__fish_seen_subcommand_from report reports" -a "state types templates add bulk update remove move clear metadata theme template save load undo redo tool"
complete -c openterminalcli -n "__fish_seen_subcommand_from excel xl spreadsheet spreadsheets" -a "read write append clear google-read google-write google-append google-clear"
complete -c openterminalcli -n "__fish_seen_subcommand_from workspace layout layouts" -a "layouts recent show export import delete rename templates template apply panels screens open tab add replace"
complete -c openterminalcli -n "__fish_seen_subcommand_from notify notification notifications" -a "providers status config set clear enable disable triggers send test"
complete -c openterminalcli -n "__fish_seen_subcommand_from scanner scan alerts" -a "list show add events delete enable disable"
complete -c openterminalcli -n "__fish_seen_subcommand_from watchlist wl" -a "list show create delete add remove lookup"
complete -c openterminalcli -n "__fish_seen_subcommand_from portfolio port" -a "list show create delete assets add sell remove tx dividend split delete-tx snapshots"
complete -c openterminalcli -n "__fish_seen_subcommand_from edge edge-radar edge_radar" -a "evaluate score kalshi-scan kalshi_scan scan-kalshi research explain kalshi-research impulse btc-impulse btc5m btc-5m polymarket-btc5m btc5m-live btc-5m-live polymarket-btc5m-live crypto-hourly crypto-hourly-live hourly-live live-crypto-hourly hourly-crypto btc-anchor add create list ls show get update close remove delete rm --category --family --timeout-ms --duration-ms --sources --window --min-confidence --direction --btc-prob --market --limit --save-candidates --min-edge --strong-edge --min-anchor --max-cost --safety-buffer --min-move-usd --max-entry-price --min-entry-seconds-left --max-entry-seconds-left --exit-before-sec --seconds-left --min-liquidity --asset-class --venue --symbol --market-id --question --market-prob --model-prob --spread --fee --liquidity --confidence --thesis --risk --tags --status --active --yes BTC-USD ETH-USD SOL-USD coinbase kraken binanceus binance bitcointicker"
complete -c openterminalcli -n "__fish_seen_subcommand_from crypto-latency latency feed-race feedrace" -a "sample snapshot watch --sources --duration-ms --min-ticks --min-live-sources BTC-USD ETH-USD SOL-USD coinbase kraken binanceus binance bitcointicker"
complete -c openterminalcli -n "__fish_seen_subcommand_from strategy strategies strat" -a "list ls templates template show info backtest bt paper-run paper run deploy --query --symbol --start --end --capital --symbols --max-iters --interval-sec --duration-sec"
complete -c openterminalcli -n "__fish_seen_subcommand_from profile account" -a "show set clear"
complete -c openterminalcli -n "__fish_seen_subcommand_from settings setting" -a "list get set clear"
complete -c openterminalcli -n "__fish_seen_subcommand_from setup onboard onboarding" -a "status checklist profile account ai provider providers doctor diagnose"
complete -c openterminalcli -n "__fish_seen_subcommand_from serve" -a "--status --stop"
complete -c openterminalcli -n "__fish_seen_subcommand_from daemon service launchd" -a "status health logs log audit security jobs job monitors monitor notify notification ai paper paper-strategy install start stop restart uninstall remove rm plist path --replace --force --start --dry-run --lines --name --every-sec --interval-sec --disabled --target --workflow --selector --cell --strategy --symbols --max-iters --provider --level --title --message --job"
