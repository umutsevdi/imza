cmake_minimum_required(VERSION 4.0)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(ursa LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -Og -Wpedantic)
    else()
        add_compile_options(-O3)
    endif()
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG})
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE})
endif()


# CURL
find_package(CURL REQUIRED)
include(FetchContent)


# ---- Dependencies ----
# FTXUI
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v7.0.3
)
FetchContent_MakeAvailable(ftxui)


# jsoncpp
set(JSONCPP_WITH_TESTS OFF)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF)
set(BUILD_STATIC_LIBS ON)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
)

# doctest
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
)

# cmark-gfm
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cmarkgfm
    GIT_REPOSITORY https://github.com/github/cmark-gfm.git
    GIT_TAG 0.29.0.gfm.13
)
FetchContent_MakeAvailable(jsoncpp doctest cmarkgfm)

file(GLOB TEST_SOURCES "test/*.cpp")

# Network transport (no ftxui, no cmark)
add_library(ursa_network
    src/network/transport.cpp
    src/network/openai_provider.cpp
    src/network/anthropic_provider.cpp
    src/network/models.cpp
)
target_include_directories(ursa_network PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_network PRIVATE
    jsoncpp_static
    CURL::libcurl
)

# Shared app configuration + catalog cache (no ftxui, no cmark)
add_library(ursa_core
    src/core/config.cpp
    src/core/catalog.cpp
)
target_include_directories(ursa_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_core PUBLIC
    ursa_network
    jsoncpp_static
)

# System/workspace environment + prompt building (no ftxui, no cmark)
add_library(ursa_environment
    src/environment/environment.cpp
    src/environment/prompt.cpp
)
target_include_directories(ursa_environment PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_environment PUBLIC
    ursa_core
    jsoncpp_static
)

add_library(ursa_provider
    src/provider/provider_store.cpp
    src/provider/pricing.cpp
)
target_include_directories(ursa_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_provider PUBLIC
    ursa_core
    ursa_network
)

# Agent: orchestration, formatting, slash commands (ftxui-free)
add_library(ursa_agent
    src/agent/attachments.cpp
    src/agent/controller.cpp
    src/agent/delegation.cpp
    src/agent/stream_loop.cpp
    src/agent/slash_commands.cpp
    src/agent/session.cpp
    src/agent/session_store.cpp
    src/agent/subagent_manager.cpp
    src/agent/format.cpp
    src/agent/tools.cpp
    src/agent/command_runner.cpp
)
target_include_directories(ursa_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_agent PUBLIC
    ursa_core
    ursa_network
    ursa_environment
    ursa_provider
    jsoncpp_static
)

# UI: ftxui components + renderers
add_library(ursa_ui
    src/ui/app.cpp
    src/ui/components.cpp
    src/ui/connect.cpp
    src/ui/markdown.cpp
    src/ui/modal.cpp
    src/ui/side_panel.cpp
    src/ui/status_line.cpp
    src/ui/tab_chat.cpp
)
target_include_directories(ursa_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${cmarkgfm_SOURCE_DIR}/src
    ${cmarkgfm_BINARY_DIR}/src
    ${cmarkgfm_SOURCE_DIR}/extensions
    ${jsoncpp_SOURCE_DIR}/include
)
target_compile_definitions(ursa_ui PRIVATE
    CMARK_GFM_STATIC_DEFINE
    CMARK_GFM_EXTENSIONS_STATIC_DEFINE
)
target_link_libraries(ursa_ui PUBLIC
    ursa_agent
    ftxui::ftxui
    libcmark-gfm-extensions_static
    libcmark-gfm_static
    jsoncpp_static
)

add_executable(ursa
    src/main.cpp
)
target_include_directories(ursa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ursa PRIVATE
    ursa_core
    ursa_ui
)

if (TESTS)
    add_executable(ursa_tests ${TEST_SOURCES})
    target_include_directories(ursa_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${cmarkgfm_SOURCE_DIR}/src
        ${cmarkgfm_BINARY_DIR}/src
        ${cmarkgfm_SOURCE_DIR}/extensions
        ${jsoncpp_SOURCE_DIR}/include
    )
    target_compile_definitions(ursa_tests PRIVATE
        CMARK_GFM_STATIC_DEFINE
        CMARK_GFM_EXTENSIONS_STATIC_DEFINE
    )
    target_link_libraries(ursa_tests PRIVATE
        ursa_agent
        ursa_ui
        doctest::doctest
    )
endif()
