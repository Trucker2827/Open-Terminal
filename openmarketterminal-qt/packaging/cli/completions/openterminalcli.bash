# bash completion for openterminalcli

_openterminalcli()
{
    local cur prev words cword
    COMPREPLY=()
    if type _init_completion >/dev/null 2>&1; then
        _init_completion || return
    else
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    fi

    local globals="--json --headless --profile --help -h"
    local commands="help status version doctor diagnose security sec setup onboard onboarding demo demos sync serve daemon service launchd screens open cmd actions action coverage profile settings mcp tools tool call quote q crypto cryptotrade ct crypto-latency latency feed-race feedrace news research macro econ economics dbnomics govdata government trade trading data datasources ds agent agents workflow workflows wf files file notes note notebook notebooks nb report reports excel xl spreadsheet spreadsheets workspace layout layouts notify notification notifications scanner scan alerts watchlist wl portfolio port edge edge-radar edge_radar playlist playlists strategy-playlist strategy-playlists strategy strategies strat hub observe ask brief risk thesis radar ai"

    case "$prev" in
        --profile)
            return 0
            ;;
        help)
            COMPREPLY=( $(compgen -W "$commands" -- "$cur") )
            return 0
            ;;
    esac

    local cmd=""
    local i
    for ((i=1; i < ${#words[@]}; i++)); do
        case "${words[i]}" in
            --json|--headless|--help|-h) ;;
            --profile) ((i++)) ;;
            *) cmd="${words[i]}"; break ;;
        esac
    done

    if [[ -z "$cmd" || $cword -le $i ]]; then
        COMPREPLY=( $(compgen -W "$globals $commands" -- "$cur") )
        return 0
    fi

    case "$cmd" in
        mcp|tools) COMPREPLY=( $(compgen -W "list search describe call" -- "$cur") ) ;;
        ai) COMPREPLY=( $(compgen -W "providers use test ask brief risk thesis radar recipes recipe run strategy" -- "$cur") ) ;;
        security|sec) COMPREPLY=( $(compgen -W "network-audit audit net --no-sockets" -- "$cur") ) ;;
        demo|demos) COMPREPLY=( $(compgen -W "trading-system system trade-system stack --symbol --crypto-symbol --horizon --market-prob --min-samples BTC BTC/USD 5m 15m 1h daily" -- "$cur") ) ;;
        recipe|playbook) COMPREPLY=( $(compgen -W "list show run" -- "$cur") ) ;;
        news) COMPREPLY=( $(compgen -W "latest search status monitors refresh mark-read save unsave" -- "$cur") ) ;;
        research) COMPREPLY=( $(compgen -W "search quote company overview history financials technicals peers news sentiment filings metrics insiders insider-summary 13f 13f-top politicians health sources source-status feeds find-company resolve gov tool --gaps --limit --period --form --months --quarters --days --force" -- "$cur") ) ;;
        macro|econ|economics|dbnomics|govdata|government) COMPREPLY=( $(compgen -W "fred bls calendar cb ons statcan census dbnomics gov econ-run run-econ tool providers datasets series observations search" -- "$cur") ) ;;
        trade|trading) COMPREPLY=( $(compgen -W "accounts drafts prepare submit cancel-draft paper live fast audit" -- "$cur") ) ;;
        crypto|cryptotrade|ct) COMPREPLY=( $(compgen -W "info exchange ticker quote q book orderbook depth candles ohlcv bars balance balances funds orders open-orders open fills trades my-trades fees fee tiers coinbase-tiers readiness ready doctor show set reset --venue maker-bps= taker-bps= rebate-pct= free-remaining= free-applies= slippage-bps= profile= buy sell order submit cancel tool --type --limit-price --price --post-only --maker --limit --timeframe --tf --symbol --yes BTC/USD ETH/USD SOL/USD BTC/USDT ETH/USDT SOL/USDT" -- "$cur") ) ;;
        data|datasources|ds) COMPREPLY=( $(compgen -W "connectors connections get create update delete stats fields test tool lake datalake data-lake status ensure manifest path mirror-edge mirror-decisions mirror-broker-events kalshi decisions broker-events query --symbol --family --horizon --call --account --tool --decision --limit --sql --csv BTC crypto event 5m 15m 1h 1d" -- "$cur") ) ;;
        files|file) COMPREPLY=( $(compgen -W "list search info read path import write download delete stats storage" -- "$cur") ) ;;
        notes|note) COMPREPLY=( $(compgen -W "list search show create update favorite archive delete export" -- "$cur") ) ;;
        notebook|notebooks|nb) COMPREPLY=( $(compgen -W "list ls show info path seed open create new run --category --difficulty --query --force --markdown --code --from-file --open --no-open --cell" -- "$cur") ) ;;
        report|reports) COMPREPLY=( $(compgen -W "state types templates add bulk update remove move clear metadata theme template save load undo redo tool" -- "$cur") ) ;;
        excel|xl|spreadsheet|spreadsheets) COMPREPLY=( $(compgen -W "read write append clear google-read google-write google-append google-clear" -- "$cur") ) ;;
        workspace|layout|layouts) COMPREPLY=( $(compgen -W "layouts recent show export import delete rename templates template apply panels screens open tab add replace" -- "$cur") ) ;;
        notify|notification|notifications) COMPREPLY=( $(compgen -W "providers status config set clear enable disable triggers send test" -- "$cur") ) ;;
        scanner|scan|alerts) COMPREPLY=( $(compgen -W "list show add events delete enable disable" -- "$cur") ) ;;
        watchlist|wl) COMPREPLY=( $(compgen -W "list show create delete add remove lookup" -- "$cur") ) ;;
        portfolio|port) COMPREPLY=( $(compgen -W "list show create delete assets add sell remove tx dividend split delete-tx snapshots" -- "$cur") ) ;;
        edge|edge-radar|edge_radar) COMPREPLY=( $(compgen -W "evaluate score microstructure micro btc-microstructure tape scalp-gate scalp scalping intraday-gate daytrade-gate spot-swing-gate swing-gate spot-gate crypto-swing spot-swing crypto-recommend crypto-recommendation recommend-crypto buy-signal crypto-universe crypto-universe-recommend universe-crypto crypto-breadth decision-cockpit trade-cockpit combined-cockpit signal-cockpit context public-context evidence-context catalysts kalshi-scan kalshi_scan scan-kalshi research explain kalshi-research snapshot-kalshi kalshi-snapshot kalshi-snapshots journal-kalshi-scan kalshi-journal-scan kalshi-decisions impulse btc-impulse btc5m btc-5m polymarket-btc5m btc5m-live btc-5m-live polymarket-btc5m-live snapshot-btc5m-live btc5m-snapshot polymarket-btc5m-snapshot crypto-hourly crypto-hourly-live hourly-live live-crypto-hourly hourly-crypto btc-anchor observe observation record train fit model models model-status probability prob predict gate meta-gate decision publish-horizons publish-outputs publish-all horizon-publish journal decisions decision-journal journal-evaluate-btc5m-live journal-btc5m-live write-btc5m-decision evaluate-btc5m-live stats summary crypto-stats crypto-accuracy crypto-table scorecard-crypto evidence paper-sim simulate proof-loop proof scorekeeping scoreboard trust trust-score regimes regime no-trade do-nothing rare-alerts alerts replay resolve why-no-trade why_no_trade no-trade readiness backfill backfill-crypto import-history collect harvest ticks cockpit dashboard terminal selftest-leakage leakage-test no-lookahead-test add create list ls show get update close remove delete rm --category --family --timeout-ms --duration-ms --sources --symbols --venue --horizon --horizon-sec --fee-bps --slippage-bps --safety-bps --min-profit-bps --minimum-profit-bps --min-edge-bps --max-symbols --window --min-confidence --direction --btc-prob --market --limit --save-candidates --min-edge --strong-edge --min-anchor --max-cost --safety-buffer --min-move-usd --max-entry-price --min-entry-seconds-left --max-entry-seconds-left --exit-before-sec --seconds-left --min-liquidity --asset-class --venue --symbol --horizon --outcome --source --days --from --to --decision-ts --market-id --question --market-prob --model-prob --spread --fee --liquidity --confidence --thesis --risk --tags --status --active --min-samples --move-5s --move-15s --move-60s --publish --train --backfill-if-needed --tick-stale-sec --market-stale-sec --model-stale-sec --train-stale-sec --min-fresh-sources --watch --collect --interval-sec --interval-ms --iterations --duration-sec --crypto-anchor --no-crypto-anchor --yes 5m 15m 1h 4h 1d daily all win loss pending BTC BTC-USD ETH-USD SOL-USD coinbase coinbase_advanced coinbase_tier2 coinbase_tier3 coinbase_tier4 coinbase_tier5 coinbase_tier6 coinbase_tier7 coinbase_tier8 coinbase_tier9 kraken kraken_pro binanceus binance alpaca_crypto bitcointicker crypto inflation fed-rates jobs macro weather politics equity commodities sports news-event other" -- "$cur") ) ;;
        crypto-latency|latency|feed-race|feedrace) COMPREPLY=( $(compgen -W "sample snapshot watch --sources --duration-ms --min-ticks --min-live-sources BTC-USD ETH-USD SOL-USD coinbase kraken binanceus binance bitcointicker" -- "$cur") ) ;;
        playlist|playlists|strategy-playlist|strategy-playlists) COMPREPLY=( $(compgen -W "templates template discover list ls show info follow add activate unfollow remove rm deactivate status state run --every-sec --no-daemon --remove-job --yes btc-hourly-edge crypto-lag-radar kalshi-universal-radar defensive-equity-watch ai-infrastructure-watch" -- "$cur") ) ;;
        strategy|strategies|strat) COMPREPLY=( $(compgen -W "playlist playlists list ls templates template show info backtest bt paper-run paper run deploy --query --symbol --start --end --capital --symbols --max-iters --interval-sec --duration-sec --every-sec --no-daemon --remove-job --yes" -- "$cur") ) ;;
        profile|account) COMPREPLY=( $(compgen -W "show set clear" -- "$cur") ) ;;
        settings|setting) COMPREPLY=( $(compgen -W "list get set clear" -- "$cur") ) ;;
        setup|onboard|onboarding) COMPREPLY=( $(compgen -W "status checklist profile account ai provider providers doctor diagnose" -- "$cur") ) ;;
        sync) COMPREPLY=( $(compgen -W "status check" -- "$cur") ) ;;
        serve) COMPREPLY=( $(compgen -W "--status --stop" -- "$cur") ) ;;
        daemon|service|launchd) COMPREPLY=( $(compgen -W "status owner who takeover claim own release health readiness ready safety trade-gate logs log audit security jobs job list history hist runs failures failed stats metrics add show run enable disable remove repair clear-running clear-failures clear-fails ack acknowledge monitors monitor scalp scalper microstructure-engine venues costs compare status state tape ticks decisions journal collectors collector feeds notify notification ai paper paper-strategy install start stop restart uninstall remove rm plist path --replace --force --kill --yes --install --start --dry-run --all --limit --lines --name --every-sec --interval-sec --timeout-sec --tick-stale-sec --market-stale-sec --model-stale-sec --train-stale-sec --min-samples --min-fresh-sources --cadence-ms --interval-ms --amounts --amount-usd --paper-amounts --sources --venue --liquidity --maker --post-only --taker --fee-bps --slippage-bps --safety-bps --min-profit-bps --minimum-profit-bps --min-net-bps --capture-ratio --max-age-ms --max-spread-bps --paper --live --disabled --target --workflow --selector --cell --strategy --symbols --max-iters --provider --level --title --message --job coinbase_advanced coinbase_tier2 coinbase_tier3 coinbase_tier4 coinbase_tier5 coinbase_tier6 coinbase_tier7 coinbase_tier8 coinbase_tier9 kraken_pro binanceus alpaca_crypto" -- "$cur") ) ;;
        *) COMPREPLY=( $(compgen -W "$globals" -- "$cur") ) ;;
    esac
}

complete -F _openterminalcli openterminalcli
