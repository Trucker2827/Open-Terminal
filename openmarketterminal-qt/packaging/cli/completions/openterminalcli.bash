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
    local commands="help status version doctor diagnose security sec setup onboard onboarding serve daemon service launchd screens open cmd actions action coverage profile settings mcp tools tool call quote q news research macro econ economics dbnomics govdata government trade trading data datasources ds agent agents workflow workflows wf files file notes note notebook notebooks nb report reports excel xl spreadsheet spreadsheets workspace layout layouts notify notification notifications scanner scan alerts watchlist wl portfolio port strategy strategies strat hub observe ask brief risk thesis radar ai"

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
        recipe|playbook) COMPREPLY=( $(compgen -W "list show run" -- "$cur") ) ;;
        news) COMPREPLY=( $(compgen -W "latest search status monitors refresh mark-read save unsave" -- "$cur") ) ;;
        research) COMPREPLY=( $(compgen -W "search quote company financials filings metrics insiders insider-summary 13f 13f-top politicians gov tool" -- "$cur") ) ;;
        macro|econ|economics|dbnomics|govdata|government) COMPREPLY=( $(compgen -W "fred bls calendar cb ons statcan census dbnomics gov econ-run run-econ tool providers datasets series observations search" -- "$cur") ) ;;
        trade|trading) COMPREPLY=( $(compgen -W "accounts drafts prepare submit cancel-draft paper live fast audit" -- "$cur") ) ;;
        data|datasources|ds) COMPREPLY=( $(compgen -W "connectors connections get create update delete stats fields test tool" -- "$cur") ) ;;
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
        strategy|strategies|strat) COMPREPLY=( $(compgen -W "list ls templates template show info backtest bt paper-run paper run deploy --query --symbol --start --end --capital --symbols --max-iters --interval-sec --duration-sec" -- "$cur") ) ;;
        profile|account) COMPREPLY=( $(compgen -W "show set clear" -- "$cur") ) ;;
        settings|setting) COMPREPLY=( $(compgen -W "list get set clear" -- "$cur") ) ;;
        setup|onboard|onboarding) COMPREPLY=( $(compgen -W "status checklist profile account ai provider providers doctor diagnose" -- "$cur") ) ;;
        serve) COMPREPLY=( $(compgen -W "--status --stop" -- "$cur") ) ;;
        daemon|service|launchd) COMPREPLY=( $(compgen -W "status health logs log audit security jobs job monitors monitor notify notification ai paper paper-strategy install start stop restart uninstall remove rm plist path --replace --force --start --dry-run --lines --name --every-sec --interval-sec --disabled --target --workflow --selector --cell --strategy --symbols --max-iters --provider --level --title --message --job" -- "$cur") ) ;;
        *) COMPREPLY=( $(compgen -W "$globals" -- "$cur") ) ;;
    esac
}

complete -F _openterminalcli openterminalcli
