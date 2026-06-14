# QtTest suite (built only when OPENMARKETTERMINAL_BUILD_TESTS=ON).
find_package(Qt6 REQUIRED COMPONENTS Test Network)

add_executable(tst_bridge_discovery_file
    tst_bridge_discovery_file.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_discovery_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_discovery_file PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_bridge_discovery_file COMMAND tst_bridge_discovery_file)

add_executable(tst_command_dispatch
    tst_command_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/CommandDispatch.cpp
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
