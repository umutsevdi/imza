cmake_minimum_required(VERSION 4.0)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(
    ursa
    VERSION 0.1.2
    HOMEPAGE_URL "https://github.com/umutsevdi/ursa"
    LANGUAGES C CXX
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/W4 /utf-8)
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

# OPTIONS
option(URSA_BUILD_TESTS "Build and run tests" YES)
option(URSA_PACKAGE "Package the executable" YES)
message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION}...\n")
message("Build Tests: ${URSA_BUILD_TESTS}")
message("Package: ${URSA_PACKAGE}")
message("Build Type: ${CMAKE_BUILD_TYPE}")

include(FetchContent)
if(WIN32)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_22_0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(curl)
else()
    find_package(CURL REQUIRED)
endif()


# ---- Dependencies ----
# FTXUI
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v7.0.3
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(ftxui)

# CLI11
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_PRECOMPILED ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.7.2
    EXCLUDE_FROM_ALL
)

# jsoncpp
set(JSONCPP_WITH_TESTS OFF)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF)
set(BUILD_STATIC_LIBS ON)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
    EXCLUDE_FROM_ALL
)

# doctest
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    EXCLUDE_FROM_ALL
)

# cmark-gfm
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
set(CMARK_TESTS OFF CACHE BOOL "Disable cmark-gfm tests" FORCE)
FetchContent_Declare(
    cmarkgfm
    GIT_REPOSITORY https://github.com/github/cmark-gfm.git
    GIT_TAG 0.29.0.gfm.13
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(jsoncpp cmarkgfm cli11)
if(URSA_BUILD_TESTS)
    FetchContent_MakeAvailable(doctest)
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(TREE_SITTER_GENERATOR
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_tree_sitter.py")
set(TREE_SITTER_VENDOR_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter")
set(TREE_SITTER_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TREE_SITTER_GENERATOR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules")
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}"
        "${TREE_SITTER_GENERATOR}"
        --root "${TREE_SITTER_VENDOR_DIR}"
        --output "${TREE_SITTER_GENERATED_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
)
include("${TREE_SITTER_GENERATED_DIR}/tree_sitter_sources.cmake")

add_subdirectory("${TREE_SITTER_VENDOR_DIR}/core"
    "${CMAKE_CURRENT_BINARY_DIR}/tree-sitter-core" EXCLUDE_FROM_ALL)
add_library(ursa_tree_sitter_grammars STATIC
    ${URSA_TREE_SITTER_GRAMMAR_SOURCES})
get_target_property(TREE_SITTER_GRAMMAR_SOURCES
    ursa_tree_sitter_grammars SOURCES)
foreach(source IN LISTS TREE_SITTER_GRAMMAR_SOURCES)
    get_filename_component(source_directory "${source}" DIRECTORY)
    set_property(SOURCE "${source}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${source_directory}")
endforeach()
if(MSVC)
    target_compile_options(ursa_tree_sitter_grammars PRIVATE /w)
else()
    target_compile_options(ursa_tree_sitter_grammars PRIVATE -w)
endif()
target_link_libraries(ursa_tree_sitter_grammars PUBLIC tree-sitter)

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

# Platform services and stateless domain support (no ftxui, no cmark)
add_library(ursa_foundation
    src/platform/command_runner.cpp
    src/platform/config.cpp
    src/platform/json_file.cpp
    src/providers/catalog.cpp
    src/providers/pricing.cpp
    src/workspace/git.cpp
)
target_include_directories(ursa_foundation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_foundation PUBLIC
    ursa_network
    jsoncpp_static
)

# Domain state and runtime infrastructure (no ftxui, no cmark)
add_library(ursa_runtime
    src/conversation/format.cpp
    src/conversation/persistence.cpp
    src/conversation/session.cpp
    src/permissions/store.cpp
    src/providers/store.cpp
    src/workspace/review.cpp
    src/runtime/main_thread_queue.cpp
    src/runtime/modal_queue.cpp
    src/runtime/subagent_manager.cpp
    src/tools/skills.cpp
    src/workspace/attachments.cpp
    src/workspace/environment.cpp
)
target_include_directories(ursa_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_runtime PUBLIC
    ursa_foundation
    ursa_network
    jsoncpp_static
)

# Application composition, turns, and tools (ftxui-free)
add_library(ursa_execution
    src/app/application_state.cpp
    src/app/cli.cpp
    src/app/flows.cpp
    src/app/one_shot.cpp
    src/app/slash_commands.cpp
    src/permissions/evaluator.cpp
    src/permissions/filesystem.cpp
    src/tools/tool.cpp
    src/tools/tool_args.cpp
    src/tools/shell.cpp
    src/tools/web.cpp
    src/turn/delegation.cpp
    src/turn/prompt.cpp
    src/turn/turn_runner.cpp
)
target_include_directories(ursa_execution PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${jsoncpp_SOURCE_DIR}/include
)
set_source_files_properties(src/ui/status_line.cpp PROPERTIES
    COMPILE_DEFINITIONS URSA_VERSION="${PROJECT_VERSION}")
set_source_files_properties(src/app/cli.cpp PROPERTIES
    COMPILE_DEFINITIONS URSA_VERSION="${PROJECT_VERSION}")
target_link_libraries(ursa_execution PRIVATE
    CLI11::CLI11
)
target_link_libraries(ursa_execution PUBLIC
    ursa_foundation
    ursa_network
    ursa_runtime
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
    src/ui/review_syntax.cpp
    src/ui/side_panel.cpp
    src/ui/status_line.cpp
    src/ui/syntax.cpp
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
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_definitions(ursa_ui PRIVATE
    CMARK_GFM_STATIC_DEFINE
    CMARK_GFM_EXTENSIONS_STATIC_DEFINE
)
target_link_libraries(ursa_ui PUBLIC
    ursa_execution
    ftxui::ftxui
    libcmark-gfm-extensions_static
    libcmark-gfm_static
    jsoncpp_static
    ursa_tree_sitter_grammars
)

if (WIN32)
    file(GLOB SOURCES src/main.cpp misc/resource.rc)
else()
    file(GLOB SOURCES src/main.cpp)
endif()
add_executable(ursa ${SOURCES})
target_include_directories(ursa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ursa PRIVATE
    ursa_foundation
    ursa_ui
)

if (URSA_BUILD_TESTS)
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
        ursa_execution
        ursa_ui
        doctest::doctest
    )
endif()
add_custom_target(bundle DEPENDS ${BUNDLE_OUTPUTS})
add_dependencies(${PROJECT_NAME} bundle)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND URSA_PACKAGE)
    if(NOT WIN32)
        set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
        set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
        set(CPACK_PACKAGE_VENDOR "Umut Sevdi")
        set(CPACK_PACKAGE_CONTACT "Umut Sevdi <mail@umutsevdi.com>")
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
            "Open source multi-modal coding agent."
        )
        set(CPACK_PACKAGE_DESCRIPTION
            "Ursa is an open source multi-modal coding agent."
        )
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

        # RPM
        set(CPACK_RPM_PACKAGE_REQUIRES "libcurl")
        set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
        set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
        set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
        set(CPACK_RPM_PACKAGE_DESCRIPTION
            "${CPACK_PACKAGE_DESCRIPTION}"
        )
        # DEB
        set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
        set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4")
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
        else()
            message(FATAL_ERROR
                "Unsupported Debian architecture: ${CMAKE_SYSTEM_PROCESSOR}"
            )
        endif()

        include(GNUInstallDirs)
        install(TARGETS ursa
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )
        install(FILES
                "${CMAKE_SOURCE_DIR}/README.md"
                "${CMAKE_SOURCE_DIR}/CHANGELOG.txt"
                "${CMAKE_SOURCE_DIR}/misc/LICENSE.thirdparty.txt"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        )
        install(
            FILES "${CMAKE_SOURCE_DIR}/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}"
            RENAME copyright
        )
        install(
            FILES "${CMAKE_SOURCE_DIR}/misc/ursa.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        )

        install(
            FILES "${CMAKE_SOURCE_DIR}/misc/icon.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps"
            RENAME ursa.png
        )
        set(CPACK_GENERATOR "DEB;RPM;TGZ")
        include(CPack)
    endif()
endif()
