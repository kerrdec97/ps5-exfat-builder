cmake_minimum_required(VERSION 3.24)

# PS5 exFAT Image Builder v5 (native C++/Qt).
# The legacy Python application at the repository root is left untouched; this
# project is self-contained under native/.

project(FarewellEdition
    VERSION 5.0.0
    DESCRIPTION "PS5 exFAT Image Builder v5 (native)"
    LANGUAGES CXX)

set(FW_PRODUCT_NAME "exFAT Image Builder")
set(FW_EDITION_NAME "v5")
set(FW_VERSION_LABEL "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FW_BUILD_TESTS "Build the automated tests" ON)
option(FW_BUILD_APP "Build the Qt Widgets application" ON)
option(FW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FwWarnings)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent Network)
if(FW_BUILD_APP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Widgets Svg)
endif()
if(FW_BUILD_TESTS)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    enable_testing()
endif()

# zlib is needed by the PFS/FFPFSC engine and the ZIP writer. Qt ships its own
# private copy, but we want a stable public API, so prefer a system/vcpkg zlib
# and fall back to fetching it.
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    include(FetchContent)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    endif()
endif()

configure_file(src/core/BuildInfo.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/BuildInfo.h @ONLY)

add_subdirectory(src)

if(FW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
