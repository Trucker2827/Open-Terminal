# QtTest suite (built only when OPENMARKETTERMINAL_BUILD_TESTS=ON).
find_package(Qt6 REQUIRED COMPONENTS Test Network Concurrent)

add_executable(tst_bridge_discovery_file
    tst_bridge_discovery_file.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_discovery_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_discovery_file PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_bridge_discovery_file COMMAND tst_bridge_discovery_file)

add_executable(tst_command_dispatch
    tst_command_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CommandDispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/AiRunCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ObserveCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/ServeCommand.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscovery.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
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
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_serve_command PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_serve_command PRIVATE openterminal_core Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME tst_serve_command COMMAND tst_serve_command)

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
