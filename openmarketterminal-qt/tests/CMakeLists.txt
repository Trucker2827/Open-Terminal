# QtTest suite (built only when OPENMARKETTERMINAL_BUILD_TESTS=ON).
find_package(Qt6 REQUIRED COMPONENTS Test Network)

add_executable(tst_bridge_discovery_file
    tst_bridge_discovery_file.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/BridgeDiscoveryFile.cpp)
target_include_directories(tst_bridge_discovery_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_bridge_discovery_file PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_bridge_discovery_file COMMAND tst_bridge_discovery_file)
