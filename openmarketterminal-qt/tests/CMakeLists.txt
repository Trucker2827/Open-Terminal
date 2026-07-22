# QtTest suite (built only when OPENMARKETTERMINAL_BUILD_TESTS=ON).
find_package(Qt6 REQUIRED COMPONENTS Test Network Concurrent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Read-only authenticated Kalshi queue-position bridge. Uses unittest so the
# packaged developer test path does not require pytest or other test extras.
add_test(NAME test_prediction_kalshi_queue
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_prediction_kalshi_queue.py -v)
add_test(NAME test_kalshi_strategy
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_kalshi_strategy.py -v)
add_test(NAME test_kalshi_advisor_core
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_kalshi_advisor_core.py -v)
add_test(NAME test_kalshi_competition
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_kalshi_competition.py -v)

add_executable(tst_bridge_discovery_file
    tst_bridge_discovery_file.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_discovery_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_discovery_file PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_bridge_discovery_file COMMAND tst_bridge_discovery_file)

add_executable(tst_command_dispatch
    tst_command_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CommandDispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/AiRunCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ObserveCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ServeCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CryptoFeedHub.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscovery.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp
    ${CMAKE_SOURCE_DIR}/src/screens/ai_chat/AnalysisSlashCommands.cpp)
target_include_directories(tst_command_dispatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
# CommandDispatch.cpp now references openterminal_core (HeadlessRuntime +
# McpProvider) for --headless support, so the test links the core lib too.
target_link_libraries(tst_command_dispatch PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME tst_command_dispatch COMMAND tst_command_dispatch)

add_executable(tst_bridge_client
    tst_bridge_client.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_client PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME tst_bridge_client COMMAND tst_bridge_client)

add_executable(tst_automation_state
    tst_automation_state.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_automation_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_automation_state PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_automation_state COMMAND tst_automation_state)

# Off-thread dispatch proof — drives the REAL TerminalMcpBridge (in
# openterminal_core) via BridgeClient against a probe tool and asserts the
# handler ran OFF the main thread, plus the CallFlagGuard thread-locals gate
# correctly on the worker. Links the core lib (bridge + McpProvider live there)
# and compiles the CLI-layer client sources.
add_executable(tst_bridge_offthread
    tst_bridge_offthread.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_offthread PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_offthread PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Concurrent Qt6::Sql Qt6::Test)
add_test(NAME tst_bridge_offthread COMMAND tst_bridge_offthread)

add_executable(tst_datahub_hook tst_datahub_hook.cpp
    ${CMAKE_SOURCE_DIR}/src/datahub/DataHub.cpp
    ${CMAKE_SOURCE_DIR}/src/core/logging/Logger.cpp)
target_include_directories(tst_datahub_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_datahub_hook PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_datahub_hook COMMAND tst_datahub_hook)

# Gate B — Core-only link proof: links register_core_tools' reachable closure
# against a Widgets-free target (openterminal_core + Qt6::Core + Qt6::Test ONLY).
# If any GUI symbol is still reachable from a core tool getter, this FAILS at
# link with an undefined Qt6::Widgets/Gui symbol.
add_executable(tst_mcp_init_split tst_mcp_init_split.cpp)
target_include_directories(tst_mcp_init_split PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_mcp_init_split PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_mcp_init_split COMMAND tst_mcp_init_split)

# HeadlessRuntime e2e — brings up DB+DataHub+core tools under QCoreApplication
# and round-trips a pure-local core tool (get_app_info). Links the Widgets-free
# core lib + Qt6::Core/Network/Test only (no Widgets) — proving the runtime is
# GUI-free.
add_executable(tst_headless_runtime tst_headless_runtime.cpp)
target_include_directories(tst_headless_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_headless_runtime PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_headless_runtime COMMAND tst_headless_runtime)

# Task 6 — the two CLI capability gates (cli.allow_settings_write,
# cli.allow_trading): helper unit tests + HeadlessRuntime live-path enforcement.
# Same Widgets-free link set as tst_headless_runtime.
add_executable(tst_settings_gate tst_settings_gate.cpp)
target_include_directories(tst_settings_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_settings_gate PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_settings_gate COMMAND tst_settings_gate)

# Paper Strategy-Loop Driver Task 1 — StrategyRunner harness. Pure fakes for the
# ToolCaller/Strategy seams; touches the real kill-switch read, so it links the
# same Widgets-free set as tst_settings_gate (Core/Network/Sql/Test).
add_executable(tst_strategy_loop tst_strategy_loop.cpp)
target_include_directories(tst_strategy_loop PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_strategy_loop PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_strategy_loop COMMAND tst_strategy_loop)

# AI paper ledger Task 1 — pure signed-position accounting core (apply_fill,
# unrealized_of). No DB/persistence; pure math over LedgerPosition/FillDelta.
add_executable(tst_ai_ledger tst_ai_ledger.cpp)
target_include_directories(tst_ai_ledger PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ai_ledger PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ai_ledger COMMAND tst_ai_ledger)

add_executable(tst_ai_scorecard tst_ai_scorecard.cpp)
target_include_directories(tst_ai_scorecard PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ai_scorecard PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ai_scorecard COMMAND tst_ai_scorecard)

# AI-Trading Substrate Phase A Task 1 — the SECURITY KEYSTONE: the settings-WRITE
# tool path must refuse any cli.* key (arm/enable trading, raise risk caps) even
# when cli.allow_settings_write is on, so a CLI/AI agent can't escalate itself.
# Same Widgets-free link set as tst_settings_gate (HeadlessRuntime live path).
add_executable(tst_trading_gate_keystone tst_trading_gate_keystone.cpp)
target_include_directories(tst_trading_gate_keystone PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_trading_gate_keystone PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_trading_gate_keystone COMMAND tst_trading_gate_keystone)

# Phase 2b Task 1 — the shared register_all_data_services() helper must bring up
# all 11 data services FAST and one-shot-safely (no socket/spawn/hang at
# registration). Widgets-free link set (Core/Network/Sql/Test) so QTEST_MAIN
# runs under a QCoreApplication — i.e. the headless mode the helper must be safe
# in.
add_executable(tst_data_services tst_data_services.cpp)
target_include_directories(tst_data_services PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_data_services PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_data_services COMMAND tst_data_services)

# Phase 3 Task 3 — serve --status / --stop decision logic. Compiles ServeCommand
# (the daemon + status/stop helpers) + BridgeDiscoveryFile and links the core lib
# (HeadlessRuntime/McpProvider/TerminalMcpBridge live there). The test redirects
# HOME to a QTemporaryDir and asserts the refuse-gui / not-running / live-daemon
# branches without ever SIGTERM-ing itself.
add_executable(tst_serve_command
    tst_serve_command.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ServeCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CryptoFeedHub.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp)
target_include_directories(tst_serve_command PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_serve_command PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_serve_command COMMAND tst_serve_command)

# Phase 2 / Track 2 — daemon paper-strategy jobs must thread F5's saved risk
# caps (--max-notional/--max-position/--max-aggregate) into the scheduled
# `strategy paper-run` subprocess. Exercises the pure command_for_job_kind()
# spec->args builder directly (no daemon I/O), same compile/link set as
# tst_serve_command since it shares the same ServeCommand.cpp translation unit.
add_executable(tst_daemon_job_caps
    tst_daemon_job_caps.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ServeCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CryptoFeedHub.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/automation/AutomationState.cpp)
target_include_directories(tst_daemon_job_caps PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_daemon_job_caps PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_daemon_job_caps COMMAND tst_daemon_job_caps)

# AI-Trading Substrate Phase A Task 2 — storage layer for the two-phase order
# flow: the v049 migration (order_drafts + trade_audit) and the two repositories
# that read/write them. Brings up a real DB under a QTemporaryDir HOME via
# HeadlessRuntime (same Widgets-free link set as tst_settings_gate).
add_executable(tst_order_flow tst_order_flow.cpp)
target_include_directories(tst_order_flow PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_order_flow PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_order_flow COMMAND tst_order_flow)

# AI-Trading Substrate Phase B Task 1 — storage + constitution readers for the
# prediction-market paper book: the v050 migration (pm_paper_account +
# pm_paper_positions), PmPaperRepository, and the SettingsGate venue helpers.
# Brings up a real DB under a QTemporaryDir HOME via HeadlessRuntime (same
# Widgets-free link set as tst_order_flow).
add_executable(tst_pm_paper tst_pm_paper.cpp)
target_include_directories(tst_pm_paper PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_pm_paper PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_pm_paper COMMAND tst_pm_paper)

# Portfolio Account Sync Task 1 — the v060 migration (portfolios.sync_source /
# synced_at / sync_error, portfolio_assets.has_cost_basis). Brings up a real
# DB under a QTemporaryDir HOME via HeadlessRuntime (same link set as
# tst_pm_paper).
add_executable(tst_portfolio_account_sync_migration tst_portfolio_account_sync_migration.cpp)
target_include_directories(tst_portfolio_account_sync_migration PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_account_sync_migration PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_portfolio_account_sync_migration COMMAND tst_portfolio_account_sync_migration)

# Trade-history sync (Portfolio Account Sync) Task 1 — the v061 migration
# (portfolio_transactions.external_id + idx_ptx_external partial unique
# index). Same bring-up as tst_portfolio_account_sync_migration.
add_executable(tst_portfolio_trade_history_migration tst_portfolio_trade_history_migration.cpp)
target_include_directories(tst_portfolio_trade_history_migration PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_trade_history_migration PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_portfolio_trade_history_migration COMMAND tst_portfolio_trade_history_migration)

# MigrationRunner skip-logic regression — synthetic migrations against a
# fresh temp-file SQLite DB, with NO register_all_migrations() call (this
# binary's global migration registry starts empty). Proves run() skips by
# per-version membership in schema_version, not by high-water mark
# (m.version <= MAX), so a migration registered later with a lower version
# number is still applied instead of being permanently skipped.
add_executable(tst_migration_runner tst_migration_runner.cpp)
target_include_directories(tst_migration_runner PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_migration_runner PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_migration_runner COMMAND tst_migration_runner)

# Trade-history sync Task 2 — PortfolioRepository::import_transaction's
# INSERT OR IGNORE dedup, keyed on v061's idx_ptx_external partial unique
# index. Same bring-up as tst_portfolio_account_sync_migration.
add_executable(tst_portfolio_import_transaction tst_portfolio_import_transaction.cpp)
target_include_directories(tst_portfolio_import_transaction PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_import_transaction PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_portfolio_import_transaction COMMAND tst_portfolio_import_transaction)

# AI-Trading Substrate Phase C Task 1 — kill switch + allowed-account
# constitution: the SettingsGate readers (cli_kill_switch_engaged /
# cli_allowed_account), the kill-switch short-circuit in BOTH order-flow
# handlers, and the keystone refusal of these GUI-only keys via set_setting.
add_executable(tst_live_trading tst_live_trading.cpp)
target_include_directories(tst_live_trading PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_live_trading PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_live_trading COMMAND tst_live_trading)

add_executable(tst_fast_live tst_fast_live.cpp)
target_include_directories(tst_fast_live PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_fast_live PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_fast_live COMMAND tst_fast_live)

# AI-Activity Feed Task 1 — TradeAuditRepository::append() publishes a
# "trade.audit" EventBus event on successful insert (fire-and-forget for the
# GUI feed/toast). Brings up a real DB via HeadlessRuntime (same Widgets-free
# link set as tst_fast_live).
add_executable(tst_ai_activity tst_ai_activity.cpp)
target_include_directories(tst_ai_activity PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ai_activity PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ai_activity COMMAND tst_ai_activity)

# IBKR conid resolution — pure parser parse_conid_from_secdef (symbol→conid
# from /iserver/secdef/search). Fixtures only, no gateway/network.
add_executable(tst_ibkr_conid tst_ibkr_conid.cpp)
target_include_directories(tst_ibkr_conid PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ibkr_conid PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_ibkr_conid COMMAND tst_ibkr_conid)

# Alpaca options v1 Task 1 — pure OCC option symbol detector. No Qt-widget deps;
# links openterminal_core (OptionSymbol.cpp lives there) so the risk floor
# (Task 2) can use it without re-linking.
add_executable(tst_option_symbol tst_option_symbol.cpp)
target_include_directories(tst_option_symbol PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_option_symbol PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_option_symbol COMMAND tst_option_symbol)

add_executable(tst_crypto_risk tst_crypto_risk.cpp)
target_include_directories(tst_crypto_risk PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_risk PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_risk COMMAND tst_crypto_risk)

add_executable(tst_layout_templates tst_layout_templates.cpp)
target_include_directories(tst_layout_templates PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_layout_templates PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_layout_templates COMMAND tst_layout_templates)

# Black-Scholes / BSM / Black option pricers (services/options/OptionPricing) —
# pure math driving the strategy payoff curve. Known-answer + invariant coverage
# (put-call parity, Black-at-forward == BS). Pricers live in openterminal_core.
add_executable(tst_option_pricing tst_option_pricing.cpp)
target_include_directories(tst_option_pricing PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_option_pricing PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_option_pricing COMMAND tst_option_pricing)

# Alpha-arena RiskEngine (services/alpha_arena/RiskEngine) — the fail-closed
# trade-safety gate (leverage / risk / liquidation / Monk caps + drift amend)
# and the per-agent circuit breaker (drawdown / parse-failure / risk-reject).
# Pure functions, live in openterminal_core.
add_executable(tst_risk_engine tst_risk_engine.cpp)
target_include_directories(tst_risk_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_risk_engine PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_risk_engine COMMAND tst_risk_engine)

# Event-driven backtester (algo_engine/BacktestEngine::run) — pure
# candles+strategy -> metrics JSON. Known-answer single-trade P&L (take-profit
# win / stop-loss loss) + insufficient-data and no-trade edges. Engine +
# ConditionEvaluator + IndicatorEngine all live in openterminal_core.
add_executable(tst_backtest_engine tst_backtest_engine.cpp)
target_include_directories(tst_backtest_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_backtest_engine PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_backtest_engine COMMAND tst_backtest_engine)

# Portfolio risk stats (services/portfolio/PortfolioStats) — Sharpe / Sortino /
# max-drawdown extracted from PortfolioService::compute_metrics. Header-only
# pure math, so the test needs only Qt6::Core (no core lib link).
add_executable(tst_portfolio_stats tst_portfolio_stats.cpp)
target_include_directories(tst_portfolio_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_stats PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_stats COMMAND tst_portfolio_stats)

# Portfolio summary builder (services/portfolio/PortfolioSummaryBuild) extracted
# from PortfolioService::finalize_summary — pure per-holding pricing/aggregation
# plus the unpriced-holding flag/tally that gates NAV snapshot persistence.
# Header-only, so the test needs only Qt6::Core (no core lib link).
add_executable(tst_portfolio_summary tst_portfolio_summary.cpp)
target_include_directories(tst_portfolio_summary PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_summary PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_summary COMMAND tst_portfolio_summary)

# Portfolio holding display helper (screens/portfolio/PortfolioHoldingDisplay)
# extracted from PortfolioBlotter — the unpriced-holding badge (em-dashes +
# muted row instead of a fabricated "+0.00%"). Header-only, Qt6::Core only.
add_executable(tst_portfolio_display tst_portfolio_display.cpp)
target_include_directories(tst_portfolio_display PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_display PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_display COMMAND tst_portfolio_display)

# Portfolio sync-status label text builder (screens/portfolio/PortfolioSyncStatus)
# — pure wording/empty-state logic for the account-sync toolbar label.
add_executable(tst_portfolio_sync_status tst_portfolio_sync_status.cpp)
target_include_directories(tst_portfolio_sync_status PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_sync_status PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_sync_status COMMAND tst_portfolio_sync_status)

# Paper/live source classifier (services/portfolio/PortfolioAccountMode) —
# excludes paper (fake-money) accounts from the All Accounts real-money total.
add_executable(tst_portfolio_account_mode tst_portfolio_account_mode.cpp)
target_include_directories(tst_portfolio_account_mode PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_account_mode PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_account_mode COMMAND tst_portfolio_account_mode)

# Agent Runner relevance filter (screens/portfolio/PortfolioAgentFilter) —
# filter the agent list to the asset classes a portfolio holds.
add_executable(tst_portfolio_agent_filter tst_portfolio_agent_filter.cpp)
target_include_directories(tst_portfolio_agent_filter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_portfolio_agent_filter PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_portfolio_agent_filter COMMAND tst_portfolio_agent_filter)

# Crypto DOM Ladder Task 1 — CryptoLadderModel::bucket_price + build()'s
# depth-only bucketing (VAP/orders overlay added in Tasks 2-3). Pure header,
# no widgets/DB/network, so only Qt6::Core + Qt6::Test are needed.
add_executable(tst_crypto_ladder_model tst_crypto_ladder_model.cpp)
target_include_directories(tst_crypto_ladder_model PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_ladder_model PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_ladder_model COMMAND tst_crypto_ladder_model)

# Portfolio Account Sync Task 4 — pure mirror-reconcile diff
# (services/portfolio/AccountSyncTypes::reconcile_mirror): current portfolio
# assets vs freshly fetched broker/exchange holdings -> add/update/remove
# plan, keyed by canonical symbol. Header-only, so the test needs only
# Qt6::Core (no core lib link), same shape as tst_portfolio_summary.
add_executable(tst_account_sync_reconcile tst_account_sync_reconcile.cpp)
target_include_directories(tst_account_sync_reconcile PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_account_sync_reconcile PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_account_sync_reconcile COMMAND tst_account_sync_reconcile)

# Portfolio Account Sync Task 6 — EquityAccountSource::fetch() over a fake
# IBroker injected via the broker-resolver callback (get_holdings/get_funds ->
# SyncedHolding list + "$CASH:<ccy>" line; get_holdings error -> ok=false, no
# partial mirror). Links the core lib for trading::IBroker/AccountManager/
# BrokerRegistry types, same link set as tst_live_trading.
add_executable(tst_equity_account_source tst_equity_account_source.cpp)
target_include_directories(tst_equity_account_source PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_equity_account_source PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_equity_account_source COMMAND tst_equity_account_source)

# Portfolio Account Sync Task 7 — AccountSyncService::sync_account() over a
# scriptable FakeSource: initial sync creates a synced portfolio mirroring
# the fetch, a re-sync mirrors exactly (add/update/remove), and a failed
# fetch records sync_error WITHOUT touching portfolio_assets (never-wipe
# invariant). Bring-up mirrors tst_portfolio_account_sync_migration.cpp
# (HeadlessRuntime DB under a QTemporaryDir HOME).
add_executable(tst_account_sync_service tst_account_sync_service.cpp)
target_include_directories(tst_account_sync_service PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_account_sync_service PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_account_sync_service COMMAND tst_account_sync_service)

# Portfolio Account Sync Task 9 — CryptoAccountSource::fetch() over an
# injected balance-fetcher callback (no live exchange daemon needed):
# {"balances":{CCY:{"total":n}}} -> SyncedHolding list (fiat CCY -> "$CASH:
# <CCY>" line, else "<CCY>-USD" coin holding) + an error/missing-balances
# response -> ok=false, no partial mirror. Links the core lib for
# trading::ExchangeSessionManager/ExchangeSession + SecureStorage types used
# by the default (non-injected) resolver, same link set as
# tst_equity_account_source.
add_executable(tst_crypto_account_source tst_crypto_account_source.cpp)
target_include_directories(tst_crypto_account_source PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_account_source PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_crypto_account_source COMMAND tst_crypto_account_source)

# Observer journal reader — pure read-only parser of the headless observer's
# trade-journal.md / observe_history.jsonl. Compiles just the service source
# (needs only Qt6::Core) against a QTemporaryDir fixture; no core lib bring-up.
add_executable(tst_observer_journal
    tst_observer_journal.cpp
    ${CMAKE_SOURCE_DIR}/src/services/observer/ObserverJournalService.cpp)
target_include_directories(tst_observer_journal PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_observer_journal PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_observer_journal COMMAND tst_observer_journal)

# Observer MCP tool — get_observations handler over ObserverJournalService.
# Compiles the tool + service sources (Qt6::Core only); invokes the handler
# directly against a QTemporaryDir fixture, no MCP provider/runtime bring-up.
add_executable(tst_observer_tools
    tst_observer_tools.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/tools/ObserverTools.cpp
    ${CMAKE_SOURCE_DIR}/src/services/observer/ObserverJournalService.cpp)
target_include_directories(tst_observer_tools PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_observer_tools PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_observer_tools COMMAND tst_observer_tools)

# Observer GUI view-selection logic — the pure (Core-only) markdown_for() that the
# QWidget panel renders. No Widgets/markdown link; fixture-driven.
add_executable(tst_observer_view
    tst_observer_view.cpp
    ${CMAKE_SOURCE_DIR}/src/screens/observer/ObserverJournalView.cpp
    ${CMAKE_SOURCE_DIR}/src/services/observer/ObserverJournalService.cpp)
target_include_directories(tst_observer_view PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_observer_view PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_observer_view COMMAND tst_observer_view)

# Analysis-skill loader — reads resources/ai_skills/*.md into a system-prompt
# fragment for the in-app LLM. Pure (Qt6::Core only), fixture-driven.
add_executable(tst_analysis_skills
    tst_analysis_skills.cpp
    ${CMAKE_SOURCE_DIR}/src/services/llm/AnalysisSkillLoader.cpp)
target_include_directories(tst_analysis_skills PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_analysis_skills PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_analysis_skills COMMAND tst_analysis_skills)

# Analysis slash-command expansion (/comps /dcf /earnings) — pure string logic.
add_executable(tst_analysis_slash
    tst_analysis_slash.cpp
    ${CMAKE_SOURCE_DIR}/src/screens/ai_chat/AnalysisSlashCommands.cpp)
target_include_directories(tst_analysis_slash PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_analysis_slash PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_analysis_slash COMMAND tst_analysis_slash)

# Session-title helpers: random placeholder, the predicate that recognizes it,
# and first-message → human-title derivation. Pure QString logic, core-only.
add_executable(tst_session_title
    tst_session_title.cpp
    ${CMAKE_SOURCE_DIR}/src/screens/ai_chat/SessionTitle.cpp)
target_include_directories(tst_session_title PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_session_title PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_session_title COMMAND tst_session_title)

# Task 4 — local-model essentials toolset: curated ~18-tool set that local/Ollama
# models receive instead of going through Tool-RAG. mcp::local_essentials_tool_names()
# lives in McpProvider.cpp (part of openterminal_core), so we link the core lib
# (mirrors tst_mcp_init_split) rather than compiling McpProvider.cpp standalone.
add_executable(tst_local_essentials tst_local_essentials.cpp)
target_include_directories(tst_local_essentials PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_local_essentials PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_local_essentials COMMAND tst_local_essentials)

# DuckDuckGo HTML-endpoint result parser — pure C++/Qt, no network in the function.
# Parses result__a anchors and result__snippet anchors, decodes DDG /l/?uddg= redirects.
add_executable(tst_web_search_parser
    tst_web_search_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/web/WebSearchParser.cpp)
target_include_directories(tst_web_search_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_web_search_parser PRIVATE Qt6::Core Qt6::Test)
# fixtures/ resolved via QFINDTESTDATA — copy next to the binary
add_custom_command(TARGET tst_web_search_parser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures $<TARGET_FILE_DIR:tst_web_search_parser>/fixtures)
add_test(NAME tst_web_search_parser COMMAND tst_web_search_parser)

# Bare-JSON tool-call salvage — lets weak/local models (llama via Ollama) whose
# tool calls arrive as plain-text JSON still drive the analysis loop. Pure parser,
# guarded to known tool names.
add_executable(tst_text_tool_call_parser
    tst_text_tool_call_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/services/llm/TextToolCallParser.cpp)
target_include_directories(tst_text_tool_call_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_text_tool_call_parser PRIVATE Qt6::Core Qt6::Test)
set_target_properties(tst_text_tool_call_parser PROPERTIES UNITY_BUILD OFF)
add_test(NAME tst_text_tool_call_parser COMMAND tst_text_tool_call_parser)

# Safety interlock — pins the Hyperliquid live order path shut while the signer's
# ECDSA recovery id is still hardcoded (v=27, correct only ~half the time). Links
# the core lib for HyperliquidVenue::place_order. See the test file header.
add_executable(tst_hyperliquid_gate tst_hyperliquid_gate.cpp)
target_include_directories(tst_hyperliquid_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_hyperliquid_gate PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_hyperliquid_gate COMMAND tst_hyperliquid_gate)

# Backtest/feed candle aggregation (N→1 bar synthesis). Pure math, linked from
# the core lib via CandleAggregation.h — no network fetcher needed.
add_executable(tst_candle_aggregation tst_candle_aggregation.cpp)
target_include_directories(tst_candle_aggregation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_candle_aggregation PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_candle_aggregation COMMAND tst_candle_aggregation)

# Kalshi evidence engine — fee-aware ladder relationships, forward labels,
# and post-settlement venue-basis reconciliation. Pure/offline and incapable
# of placing orders.
add_executable(tst_kalshi_evidence tst_kalshi_evidence.cpp)
target_include_directories(tst_kalshi_evidence PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_kalshi_evidence PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_kalshi_evidence COMMAND tst_kalshi_evidence)

# Coherent Kalshi hourly probability surface, constrained portfolio selection,
# no-lookahead replay, and API compatibility audit. Pure and paper-only.
add_executable(tst_kalshi_auto_engine tst_kalshi_auto_engine.cpp)
target_include_directories(tst_kalshi_auto_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_kalshi_auto_engine PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_kalshi_auto_engine COMMAND tst_kalshi_auto_engine)

add_executable(tst_decision_orchestrator tst_decision_orchestrator.cpp)
target_include_directories(tst_decision_orchestrator PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_decision_orchestrator PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_decision_orchestrator COMMAND tst_decision_orchestrator)

add_executable(tst_btc_news_pulse tst_btc_news_pulse.cpp)
target_include_directories(tst_btc_news_pulse PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_btc_news_pulse PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_btc_news_pulse COMMAND tst_btc_news_pulse)

add_executable(tst_bitcoin_evidence_engine tst_bitcoin_evidence_engine.cpp)
target_include_directories(tst_bitcoin_evidence_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bitcoin_evidence_engine PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_bitcoin_evidence_engine COMMAND tst_bitcoin_evidence_engine)

# Strategy Sandbox Task 1 — migration v056 schema (sandbox_strategy,
# sandbox_position, sandbox_fill, sandbox_score). Opens a real profile DB
# through the normal migration path under a temp $HOME (pattern of
# tst_data_services.cpp) and proves every load-bearing column plus the
# structural UNIQUE dedup on sandbox_position.decision_id.
add_executable(tst_sandbox_schema tst_sandbox_schema.cpp)
target_include_directories(tst_sandbox_schema PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_schema PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_schema COMMAND tst_sandbox_schema)

add_executable(tst_strategy_registry tst_strategy_registry.cpp)
target_include_directories(tst_strategy_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_strategy_registry PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME tst_strategy_registry COMMAND tst_strategy_registry)

# AI Handler Plugin Core Task 2 — AiHandlerRepository: CRUD over migration
# v063's ai_handler table (named AI-handler configs, disarmed by default).
# Same DB bootstrap as tst_sandbox_registry.
add_executable(tst_ai_handler_repository tst_ai_handler_repository.cpp)
target_include_directories(tst_ai_handler_repository PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_ai_handler_repository PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_ai_handler_repository COMMAND tst_ai_handler_repository)

add_executable(tst_feed_reconnect tst_feed_reconnect.cpp)
target_include_directories(tst_feed_reconnect PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_feed_reconnect PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_feed_reconnect COMMAND tst_feed_reconnect)

add_executable(tst_sandbox_maker_quotes tst_sandbox_maker_quotes.cpp)
target_include_directories(tst_sandbox_maker_quotes PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_maker_quotes PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_sandbox_maker_quotes COMMAND tst_sandbox_maker_quotes)

# Strategy Sandbox Task 2 — SandboxRegistry: immutable, param-hashed strategy
# books on top of sandbox_strategy. Same DB bootstrap as tst_sandbox_schema.
add_executable(tst_sandbox_registry tst_sandbox_registry.cpp)
target_include_directories(tst_sandbox_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_registry PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_registry COMMAND tst_sandbox_registry)

# Strategy Sandbox Task 3 — TickTail: tail reader over scalp_ticks.jsonl (+
# .1 rotation predecessor) for the fill model. Pure file reader, no DB.
add_executable(tst_sandbox_ticktail tst_sandbox_ticktail.cpp)
target_include_directories(tst_sandbox_ticktail PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_ticktail PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_sandbox_ticktail COMMAND tst_sandbox_ticktail)

# Strategy Sandbox Task 4 — PaperFillModel: pure fill/exit rules for the
# paper executor. No DB, no files; Qt6::Sql is linked only because
# openterminal_core drags it in transitively.
add_executable(tst_sandbox_fillmodel tst_sandbox_fillmodel.cpp)
target_include_directories(tst_sandbox_fillmodel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_fillmodel PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_fillmodel COMMAND tst_sandbox_fillmodel)

# Strategy Sandbox Task 5 — PaperExecutor::run_cycle: candidate selection/
# opening per strategy kind, pending_fill -> open|unfilled, open -> closed.
# Same DB bootstrap as tst_sandbox_registry/tst_sandbox_schema, plus fixture
# ticks/decisions files under a per-test QTemporaryDir daemon_dir.
add_executable(tst_sandbox_executor tst_sandbox_executor.cpp)
target_include_directories(tst_sandbox_executor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_executor PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_executor COMMAND tst_sandbox_executor)

# FreshnessGate.h — pure data_quality_from_freshness/freshness_field helper,
# extracted from PaperExecutor.cpp's anon namespace for reuse (ai ctx
# decision-packet task 1). Header-only, so this target just needs Qt Core.
add_executable(tst_freshness_gate tst_freshness_gate.cpp)
target_include_directories(tst_freshness_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_freshness_gate PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_freshness_gate COMMAND tst_freshness_gate)

# ai ctx decision-packet Task 2 — DecisionContext::assess: pure-read packet
# assembly over edge_decision_journal + FreshnessGate.h + SandboxScorer.h's
# evaluate_lane_significance. Same DB bootstrap as tst_sandbox_registry.
add_executable(tst_decision_context tst_decision_context.cpp)
target_include_directories(tst_decision_context PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_decision_context PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_decision_context COMMAND tst_decision_context)

# ai screen shortlist Task 1 — Screener::screen: ranked gate-vetted shortlist
# over edge_decision_journal, reusing DecisionContext::assess per (symbol,
# venue). Same DB bootstrap as tst_decision_context.
add_executable(tst_screener tst_screener.cpp)
target_include_directories(tst_screener PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_screener PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_screener COMMAND tst_screener)

# Strategy Sandbox Task 8 — SandboxResolver::resolve_pending: settle
# prediction (journal outcome) and hypothetical (tick target/stop/expiry)
# books. Same DB bootstrap as tst_sandbox_executor.
add_executable(tst_sandbox_resolver tst_sandbox_resolver.cpp)
target_include_directories(tst_sandbox_resolver PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_resolver PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_resolver COMMAND tst_sandbox_resolver)

# Strategy Sandbox Task 9 — SandboxScorer::score_all / leaderboard: sandbox_score
# day-bucketed rollups and the season-to-date leaderboard aggregation. Same DB
# bootstrap as tst_sandbox_resolver.
add_executable(tst_sandbox_scorer tst_sandbox_scorer.cpp)
target_include_directories(tst_sandbox_scorer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_scorer PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_sandbox_scorer COMMAND tst_sandbox_scorer)

# Strategy Sandbox Task 10 — SandboxEligibility::evaluate_eligibility: pure,
# report-only spec-§4.7 boundary matrix. No DB bring-up needed (unlike its
# sandbox siblings above) -- evaluate_eligibility takes a plain
# EligibilityInput struct and returns a verdict, no I/O at all.
add_executable(tst_sandbox_eligibility tst_sandbox_eligibility.cpp)
target_include_directories(tst_sandbox_eligibility PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_sandbox_eligibility PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_sandbox_eligibility COMMAND tst_sandbox_eligibility)

add_executable(tst_strategy_evidence_presentation tst_strategy_evidence_presentation.cpp)
target_include_directories(tst_strategy_evidence_presentation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_strategy_evidence_presentation PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_strategy_evidence_presentation COMMAND tst_strategy_evidence_presentation)

add_executable(tst_strategy_cockpit_navigation tst_strategy_cockpit_navigation.cpp)
target_include_directories(tst_strategy_cockpit_navigation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_strategy_cockpit_navigation PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME tst_strategy_cockpit_navigation COMMAND tst_strategy_cockpit_navigation)

# AI pre-trade guardrail Task 1 — evaluate_pretrade: pure, no DB/settings/I-O.
# Takes a TradeIntent + GateInputs + GatePolicy and returns a GateVerdict.
add_executable(tst_pretrade_gate tst_pretrade_gate.cpp)
target_include_directories(tst_pretrade_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_pretrade_gate PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_pretrade_gate COMMAND tst_pretrade_gate)

add_executable(tst_deterministic_floor tst_deterministic_floor.cpp)
target_include_directories(tst_deterministic_floor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_deterministic_floor PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_deterministic_floor COMMAND tst_deterministic_floor)

add_executable(tst_typed_action tst_typed_action.cpp)
target_include_directories(tst_typed_action PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_typed_action PRIVATE openterminal_core Qt6::Core Qt6::Test)
add_test(NAME tst_typed_action COMMAND tst_typed_action)

add_executable(tst_advisory_repository tst_advisory_repository.cpp)
target_include_directories(tst_advisory_repository PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_advisory_repository PRIVATE openterminal_core Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME tst_advisory_repository COMMAND tst_advisory_repository)

# Strategy Sandbox Task 11 — power-boundary check: the sandbox service
# object files (PaperExecutor/PaperFillModel/SandboxResolver/SandboxScorer/
# SandboxEligibility/SandboxRegistry/TickTail, under
# CMakeFiles/openterminal_core.dir/src/services/sandbox/) must never
# reference the live order-placement surface (ExchangeService,
# place_exchange_order, crypto_submit_order, place_order) in their
# undefined-symbol table. REQUIRED (not non-blocking): this is deterministic,
# offline, and the whole point of Task 11 is that it gates merges. BUILD_DIR
# is passed via `cmake -E env` since ctest's working directory is not
# guaranteed to be the build tree root.
add_test(NAME sandbox_boundary_check
         COMMAND ${CMAKE_COMMAND} -E env "BUILD_DIR=${CMAKE_BINARY_DIR}"
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_boundary_check.sh)

# Strategy Sandbox Task 11 — end-to-end smoke over the built openterminalcli
# (offline, temp HOME, no GUI/daemon process): seed -> fixture scalp
# decision+ticks -> sandbox tick (open+fill) -> sandbox tick (target exit) ->
# positions --closed -> score-now/leaderboard (resolved + hand-computed
# net_pnl) -> eligibility (scalp book BLOCKED, insufficient sample).
# REQUIRED (not non-blocking) per the Task 11 brief: deterministic and
# offline, so there is no excuse for it to be advisory-only.
add_test(NAME e2e_sandbox_smoke
         COMMAND ${CMAKE_COMMAND} -E env "BUILD_DIR=${CMAKE_BINARY_DIR}"
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e_sandbox_smoke.sh)

# LLM advisory scoring Task 2 — protocol primitives (canonical JSON hash,
# horizon-aware TTL policy, allowlist-only blind-packet builder). Pure
# functions in services/edge_radar/AdvisoryProtocol, no DB/Widgets.
add_executable(tst_advisory_protocol tst_advisory_protocol.cpp)
target_include_directories(tst_advisory_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_advisory_protocol PRIVATE openterminal_core Qt6::Test)
add_test(NAME tst_advisory_protocol COMMAND tst_advisory_protocol)

add_executable(tst_advisory_scoring tst_advisory_scoring.cpp)
target_include_directories(tst_advisory_scoring PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_advisory_scoring PRIVATE openterminal_core Qt6::Test)
add_test(NAME tst_advisory_scoring COMMAND tst_advisory_scoring)

add_executable(tst_advisor_canary_presentation tst_advisor_canary_presentation.cpp)
target_include_directories(tst_advisor_canary_presentation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_advisor_canary_presentation PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_advisor_canary_presentation COMMAND tst_advisor_canary_presentation)

add_executable(tst_crypto_symbol_universe tst_crypto_symbol_universe.cpp)
target_include_directories(tst_crypto_symbol_universe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_symbol_universe PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_symbol_universe COMMAND tst_crypto_symbol_universe)

add_executable(tst_crypto_chrome_state tst_crypto_chrome_state.cpp)
target_include_directories(tst_crypto_chrome_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_crypto_chrome_state PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_crypto_chrome_state COMMAND tst_crypto_chrome_state)
