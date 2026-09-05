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
FetchContent_MakeAvailable(jsoncpp doctest cmarkgfm)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
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


set(PKGDIR "${CMAKE_BINARY_DIR}/package")
function(add_bundle_file source destination)
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    add_custom_command(
        OUTPUT "${destination}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${destination_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${destination}"
        DEPENDS "${source}"
        VERBATIM
    )
    set(BUNDLE_OUTPUTS ${BUNDLE_OUTPUTS} "${destination}" PARENT_SCOPE)
endfunction()

add_bundle_file("${CMAKE_SOURCE_DIR}/misc/ursa.ico"
    "${PKGDIR}/win/ursa.ico")
add_bundle_file("${CMAKE_SOURCE_DIR}/CHANGELOG.txt"
    "${PKGDIR}/win/CHANGELOG.txt")
add_bundle_file("${CMAKE_SOURCE_DIR}/LICENSE"
    "${PKGDIR}/win/LICENSE")
add_bundle_file("${CMAKE_SOURCE_DIR}/misc/LICENSE.thirdparty.txt"
    "${PKGDIR}/win/LICENSE.thirdparty.txt")
add_bundle_file("${CMAKE_SOURCE_DIR}/LICENSE"
    "${PKGDIR}/usr/share/${PROJECT_NAME}/LICENSE")
add_bundle_file("${CMAKE_SOURCE_DIR}/misc/LICENSE.thirdparty.txt"
    "${PKGDIR}/usr/share/${PROJECT_NAME}/LICENSE.thirdparty.txt")
add_bundle_file("${CMAKE_SOURCE_DIR}/misc/icon.png"
    "${PKGDIR}/usr/share/${PROJECT_NAME}/ursa.png")
add_bundle_file("${CMAKE_SOURCE_DIR}/misc/ursa.desktop"
    "${PKGDIR}/usr/share/applications/ursa.desktop")
add_bundle_file("${CMAKE_SOURCE_DIR}/CHANGELOG.txt"
    "${PKGDIR}/usr/share/${PROJECT_NAME}/CHANGELOG.txt")

add_custom_target(bundle DEPENDS ${BUNDLE_OUTPUTS})
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
