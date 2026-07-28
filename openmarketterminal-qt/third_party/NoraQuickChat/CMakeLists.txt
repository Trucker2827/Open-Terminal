cmake_minimum_required(VERSION 3.21)

project(NoraQuickChat VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(NoraQuickChat STATIC
    include/NoraQuickChat/NoraAvatarWidget.h
    src/NoraAvatarWidget.cpp
    resources/nora.qrc
)

add_library(NoraQuickChat::NoraQuickChat ALIAS NoraQuickChat)

target_include_directories(NoraQuickChat
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(NoraQuickChat PUBLIC Qt6::Widgets)

set_target_properties(NoraQuickChat PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
