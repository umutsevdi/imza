cmake_minimum_required(VERSION 4.0)
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
FetchContent_Declare(
    replxx
    GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
    GIT_TAG master
)
FetchContent_MakeAvailable(replxx)


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

# cmark
FetchContent_Declare(
    cmark
    GIT_REPOSITORY https://github.com/commonmark/cmark.git
    GIT_TAG 0.31.1   # pin a tag
)
FetchContent_MakeAvailable(jsoncpp doctest cmark)


file(GLOB SOURCES "src/*.cpp")
file(GLOB TEST_SOURCES  "test/*.cpp")
add_executable(ursa ${SOURCES})
target_include_directories(ursa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${cmark_SOURCE_DIR}/src
    ${jsoncpp_SOURCE_DIR}/include
    ${replxx_SOURCE_DIR}/include
)
target_link_libraries(ursa PRIVATE
    cmark
    jsoncpp_static
    CURL::libcurl
    replxx
)

if (TESTS)
    add_executable(ursa_tests ${TEST_SOURCES})
    target_include_directories(ursa_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${cmark_SOURCE_DIR}/src
        ${jsoncpp_SOURCE_DIR}/include
        ${replxx_SOURCE_DIR}/include
    )
    target_link_libraries(ursa_tests PRIVATE
        cmark
        jsoncpp_static
        CURL::libcurl
        replxx
        doctest::doctest
    )

endif()


