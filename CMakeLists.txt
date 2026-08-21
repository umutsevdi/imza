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


# OpenSSL
find_package(OpenSSL REQUIRED)
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

# Core
add_library(ursa_core
    src/config.cpp
    src/network/network_common.cpp
    src/network/provider.cpp
    src/network/openai_provider.cpp
    src/network/anthropic_provider.cpp
)
target_include_directories(ursa_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${cmarkgfm_SOURCE_DIR}/src
    ${cmarkgfm_BINARY_DIR}/src
    ${cmarkgfm_SOURCE_DIR}/extensions
    ${jsoncpp_SOURCE_DIR}/include
)
target_link_libraries(ursa_core PRIVATE
    jsoncpp_static
    CURL::libcurl
)

add_executable(ursa
    src/main.cpp
    src/ui/controller.cpp
    src/ui/app.cpp
    src/ui/chat.cpp
    src/ui/commands.cpp
    src/ui/todo.cpp
    src/ui/changed_files.cpp
    src/ui/settings.cpp
    src/ui/question.cpp
    src/ui/toolcall.cpp
    src/render/markdown.cpp
    src/render/items.cpp
)
target_include_directories(ursa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${cmarkgfm_SOURCE_DIR}/src
    ${cmarkgfm_BINARY_DIR}/src
    ${cmarkgfm_SOURCE_DIR}/extensions
    ${jsoncpp_SOURCE_DIR}/include
)
target_compile_definitions(ursa PRIVATE
    CMARK_GFM_STATIC_DEFINE
    CMARK_GFM_EXTENSIONS_STATIC_DEFINE
)
target_link_libraries(ursa PRIVATE
    ursa_core
    ftxui::ftxui
    libcmark-gfm-extensions_static
    libcmark-gfm_static
    jsoncpp_static
    CURL::libcurl
)


    if (TESTS)
        add_executable(ursa_tests ${TEST_SOURCES}
            src/ui/commands.cpp
            src/render/markdown.cpp
            src/render/items.cpp
        )
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
        ursa_core
        ftxui::ftxui
        libcmark-gfm-extensions_static
        libcmark-gfm_static
        jsoncpp_static
        CURL::libcurl
        doctest::doctest
    )

endif()
