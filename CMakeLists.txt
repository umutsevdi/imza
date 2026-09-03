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
    src/network/json_io.cpp
    src/network/stream_event.cpp
    src/network/transport.cpp
    src/network/openai_provider.cpp
    src/network/anthropic_provider.cpp
    src/network/models.cpp
    src/network/web.cpp
)
target_include_directories(ursa_network PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_network PRIVATE
    jsoncpp_static
    CURL::libcurl
)

# Shared app configuration + catalog/pricing cache (no ftxui, no cmark)
add_library(ursa_core
    src/core/catalog.cpp
    src/core/command_runner.cpp
    src/core/config.cpp
    src/core/git.cpp
    src/core/io.cpp
    src/core/pricing.cpp
)
target_include_directories(ursa_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_core PUBLIC
    ursa_network
    jsoncpp_static
)

# Long-living state owned by ApplicationState (no ftxui, no cmark)
add_library(ursa_subsystems
    src/subsystems/attachments.cpp
    src/subsystems/environment.cpp
    src/subsystems/format.cpp
    src/subsystems/provider_store.cpp
    src/subsystems/review.cpp
    src/subsystems/session.cpp
    src/subsystems/session_store.cpp
    src/subsystems/skill_store.cpp
    src/subsystems/subagent_manager.cpp
)
target_include_directories(ursa_subsystems PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_subsystems PUBLIC
    ursa_core
    ursa_network
    jsoncpp_static
)

# Agent: orchestration, prompts, slash commands (ftxui-free)
add_library(ursa_agent
    src/agent/application_state.cpp
    src/agent/delegation_runner.cpp
    src/agent/flows.cpp
    src/agent/modal_queue.cpp
    src/agent/prompt.cpp
    src/agent/slash_commands.cpp
    src/agent/tool_args.cpp
    src/agent/tools.cpp
    src/agent/turn_runner.cpp
    src/agent/web_tools.cpp
)
target_include_directories(ursa_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_agent PUBLIC
    ursa_core
    ursa_network
    ursa_subsystems
    jsoncpp_static
)

# UI: ftxui components + renderers
add_library(ursa_ui
    src/ui/app.cpp
    src/ui/autocomplete.cpp
    src/ui/components.cpp
    src/ui/markdown.cpp
    src/ui/modals/connect.cpp
    src/ui/modals/sessions.cpp
    src/ui/modals/skills.cpp
    src/ui/modals/subagents.cpp
    src/ui/modals/tools.cpp
    src/ui/modals/variant.cpp
    src/ui/side_panel.cpp
    src/ui/status_line.cpp
    src/ui/tab_chat.cpp
    src/ui/tab_review.cpp
    src/ui/tool_format.cpp
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
