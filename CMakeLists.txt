cmake_minimum_required(VERSION 4.0)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(ursa LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (WIN32)
    message(${CMAKE_SOURCE_DIR})
    # If Win32 use vcpkg to get dependencies.
    set(CMAKE_TOOLCHAIN_FILE
       "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

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

# OPTIONS
option(URSA_BUILD_TESTS "Build and run tests" YES)
option(URSA_PACKAGE "Package the executable" YES)
message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION}...\n")
message("Build Tests: ${URSA_BUILD_TESTS}")
message("Package: ${URSA_PACKAGE}")
message("Build Type: ${CMAKE_BUILD_TYPE}")

# CURL
find_package(CURL REQUIRED)
include(FetchContent)


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
FetchContent_Declare(
    cmarkgfm
    GIT_REPOSITORY https://github.com/github/cmark-gfm.git
    GIT_TAG 0.29.0.gfm.13
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    tree_sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.26.11
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    tree_sitter_c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG v0.24.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR ursa_no_cmake
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    tree_sitter_cpp
    URL https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
    URL_HASH SHA256=7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR ursa_no_cmake
    EXCLUDE_FROM_ALL
)
function(declare_tree_sitter_grammar name repository tag)
    set(shallow TRUE)
    if(tag MATCHES "^[0-9a-f]{40}$")
        set(shallow FALSE)
    endif()
    FetchContent_Declare(
        ${name}
        GIT_REPOSITORY ${repository}
        GIT_TAG ${tag}
        GIT_SHALLOW ${shallow}
        GIT_SUBMODULES ""
        SOURCE_SUBDIR ursa_no_cmake
        EXCLUDE_FROM_ALL
    )
endfunction()
declare_tree_sitter_grammar(tree_sitter_html
    https://github.com/tree-sitter/tree-sitter-html.git v0.23.2)
declare_tree_sitter_grammar(tree_sitter_css
    https://github.com/tree-sitter/tree-sitter-css.git v0.25.0)
declare_tree_sitter_grammar(tree_sitter_javascript
    https://github.com/tree-sitter/tree-sitter-javascript.git v0.25.0)
declare_tree_sitter_grammar(tree_sitter_typescript
    https://github.com/tree-sitter/tree-sitter-typescript.git v0.23.2)
declare_tree_sitter_grammar(tree_sitter_go
    https://github.com/tree-sitter/tree-sitter-go.git v0.25.0)
declare_tree_sitter_grammar(tree_sitter_rust
    https://github.com/tree-sitter/tree-sitter-rust.git v0.24.2)
declare_tree_sitter_grammar(tree_sitter_swift
    https://github.com/alex-pinkus/tree-sitter-swift.git
    0.7.3-with-generated-files)
declare_tree_sitter_grammar(tree_sitter_dockerfile
    https://github.com/camdencheek/tree-sitter-dockerfile.git v0.2.0)
declare_tree_sitter_grammar(tree_sitter_java
    https://github.com/tree-sitter/tree-sitter-java.git v0.23.5)
declare_tree_sitter_grammar(tree_sitter_cmake
    https://github.com/uyha/tree-sitter-cmake.git v0.7.4)
declare_tree_sitter_grammar(tree_sitter_dart
    https://github.com/UserNobody14/tree-sitter-dart.git
    be07cf7118d3dba06236a3f19541685a68209934)
declare_tree_sitter_grammar(tree_sitter_make
    https://github.com/alemuller/tree-sitter-make.git
    a4b9187417d6be349ee5fd4b6e77b4172c6827dd)
declare_tree_sitter_grammar(tree_sitter_json
    https://github.com/tree-sitter/tree-sitter-json.git v0.24.8)
declare_tree_sitter_grammar(tree_sitter_lua
    https://github.com/tree-sitter-grammars/tree-sitter-lua.git v0.5.0)
declare_tree_sitter_grammar(tree_sitter_python
    https://github.com/tree-sitter/tree-sitter-python.git v0.25.0)
declare_tree_sitter_grammar(tree_sitter_php
    https://github.com/tree-sitter/tree-sitter-php.git v0.24.2)
declare_tree_sitter_grammar(tree_sitter_bash
    https://github.com/tree-sitter/tree-sitter-bash.git v0.25.1)
declare_tree_sitter_grammar(tree_sitter_powershell
    https://github.com/airbus-cert/tree-sitter-powershell.git v0.26.5)
FetchContent_MakeAvailable(jsoncpp doctest cmarkgfm tree_sitter tree_sitter_c
    tree_sitter_cpp tree_sitter_html tree_sitter_css tree_sitter_javascript
    tree_sitter_typescript tree_sitter_go tree_sitter_rust tree_sitter_swift
    tree_sitter_dockerfile tree_sitter_java tree_sitter_cmake
    tree_sitter_dart tree_sitter_make tree_sitter_json tree_sitter_lua
    tree_sitter_python tree_sitter_php tree_sitter_bash tree_sitter_powershell)

function(embed_tree_sitter_query name)
    set(query "")
    foreach(path IN LISTS ARGN)
        file(READ "${path}" part)
        string(APPEND query "${part}\n")
    endforeach()
    string(APPEND TREE_SITTER_HIGHLIGHTS_SOURCE
        "constexpr std::string_view ${name}_HIGHLIGHTS = R\"ursa_query(\n${query})ursa_query\";\n")
    set(TREE_SITTER_HIGHLIGHTS_SOURCE
        "${TREE_SITTER_HIGHLIGHTS_SOURCE}" PARENT_SCOPE)
endfunction()

embed_tree_sitter_query(CPP
    "${tree_sitter_c_SOURCE_DIR}/queries/highlights.scm"
    "${tree_sitter_cpp_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(HTML
    "${tree_sitter_html_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(CSS
    "${tree_sitter_css_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(JAVASCRIPT
    "${tree_sitter_javascript_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(TYPESCRIPT
    "${tree_sitter_javascript_SOURCE_DIR}/queries/highlights.scm"
    "${tree_sitter_typescript_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(GO
    "${tree_sitter_go_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(RUST
    "${tree_sitter_rust_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(SWIFT
    "${tree_sitter_swift_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(DOCKERFILE
    "${tree_sitter_dockerfile_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(JAVA
    "${tree_sitter_java_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(CMAKE
    "${tree_sitter_cmake_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(DART
    "${tree_sitter_dart_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(MAKE
    "${tree_sitter_make_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(JSON
    "${tree_sitter_json_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(LUA
    "${tree_sitter_lua_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(PYTHON
    "${tree_sitter_python_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(PHP
    "${tree_sitter_php_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(BASH
    "${tree_sitter_bash_SOURCE_DIR}/queries/highlights.scm")
embed_tree_sitter_query(POWERSHELL
    "${tree_sitter_powershell_SOURCE_DIR}/queries/highlights.scm")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/syntax_queries.inc"
    CONTENT "${TREE_SITTER_HIGHLIGHTS_SOURCE}")

add_library(ursa_tree_sitter_grammars STATIC
    ${tree_sitter_cpp_SOURCE_DIR}/src/parser.c
    ${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_html_SOURCE_DIR}/src/parser.c
    ${tree_sitter_html_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_css_SOURCE_DIR}/src/parser.c
    ${tree_sitter_css_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_javascript_SOURCE_DIR}/src/parser.c
    ${tree_sitter_javascript_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_typescript_SOURCE_DIR}/typescript/src/parser.c
    ${tree_sitter_typescript_SOURCE_DIR}/typescript/src/scanner.c
    ${tree_sitter_go_SOURCE_DIR}/src/parser.c
    ${tree_sitter_rust_SOURCE_DIR}/src/parser.c
    ${tree_sitter_rust_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_swift_SOURCE_DIR}/src/parser.c
    ${tree_sitter_swift_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_dockerfile_SOURCE_DIR}/src/parser.c
    ${tree_sitter_dockerfile_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_java_SOURCE_DIR}/src/parser.c
    ${tree_sitter_cmake_SOURCE_DIR}/src/parser.c
    ${tree_sitter_cmake_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_dart_SOURCE_DIR}/src/parser.c
    ${tree_sitter_dart_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_make_SOURCE_DIR}/src/parser.c
    ${tree_sitter_json_SOURCE_DIR}/src/parser.c
    ${tree_sitter_lua_SOURCE_DIR}/src/parser.c
    ${tree_sitter_lua_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_python_SOURCE_DIR}/src/parser.c
    ${tree_sitter_python_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_php_SOURCE_DIR}/php/src/parser.c
    ${tree_sitter_php_SOURCE_DIR}/php/src/scanner.c
    ${tree_sitter_bash_SOURCE_DIR}/src/parser.c
    ${tree_sitter_bash_SOURCE_DIR}/src/scanner.c
    ${tree_sitter_powershell_SOURCE_DIR}/src/parser.c
    ${tree_sitter_powershell_SOURCE_DIR}/src/scanner.c
)
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
    src/subsystems/delegation_runner.cpp
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
    ursa_agent
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
    ursa_core
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
        ursa_agent
        ursa_ui
        doctest::doctest
    )
endif()


add_custom_target(bundle)
set(PKGDIR "${CMAKE_BINARY_DIR}/package")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/misc/ursa.ico
                "${PKGDIR}/win/ursa.ico")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/CHANGELOG.txt
        "${PKGDIR}/win/")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/LICENSE
        "${PKGDIR}/win/")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/misc/LICENSE.tree-sitter
        "${PKGDIR}/win/")


add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/LICENSE
                "${PKGDIR}/usr/share/${PROJECT_NAME}/LICENSE")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/misc/LICENSE.tree-sitter
                "${PKGDIR}/usr/share/${PROJECT_NAME}/LICENSE.tree-sitter")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/misc/icon.png
                "${PKGDIR}/usr/share/${PROJECT_NAME}/ursa.png")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/misc/ursa.desktop
                "${PKGDIR}/usr/share/applications/ursa.desktop")
add_custom_command(TARGET bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/CHANGELOG.txt "${PKGDIR}/usr/share/${PROJECT_NAME}")
add_dependencies(${PROJECT_NAME} bundle)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND URSA_PACKAGE)
    if(NOT WIN32)
        set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
        set(CPACK_PACKAGE_VERSION "0.1.0")
        set(CPACK_PACKAGE_DESCRIPTION "Open source multi-modal coding agent.")
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${CPACK_PACKAGE_DESCRIPTION})
        set(CPACK_PACKAGE_VENDOR "Umut Sevdi")
        set(CPACK_PACKAGE_MAINTAINER "Umut Sevdi <mail@umutsevdi.com>")
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

        set(CPACK_GENERATOR "DEB;RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "GPL")
        set(CPACK_RPM_PACKAGE_GROUP "Development")
        set(CPACK_RPM_PACKAGE_REQUIRES "libcurl")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Umut Sevdi <mail@umutsevdi.com>")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4")

        install(FILES "build/release/ursa"
    DESTINATION "/usr/bin/"
    PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
          GROUP_EXECUTE GROUP_READ
          WORLD_EXECUTE WORLD_READ
  )

        install(DIRECTORY ${PKGDIR}/usr/share/
    DESTINATION /usr/share/
    USE_SOURCE_PERMISSIONS
    PATTERN "*.*"
  )

        install(FILES "LICENSE"
   DESTINATION "/usr/share/ursa"
  )
        include(CPack)
    endif()
endif()
