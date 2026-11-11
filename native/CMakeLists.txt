cmake_minimum_required(VERSION 3.24)

# PS5 exFAT Image Builder v5 (native C++/Qt). The project is self-contained
# under native/.

project(FarewellEdition
    VERSION 5.0.3
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

# zstd (BSD) and LZ4 (BSD-2-Clause) for game asset packs: zstd packs
# (ampr-emu-ng AMPRZPK1) are written and read; LZ4 packs (ampr_emu AMPRPAK4)
# are only read, so LZ4-packed games can be unpacked (the app never writes
# LZ4 packs). Only the libraries' C sources are compiled (no programs, no
# assembly, no legacy formats); the source archives are pinned by hash.
enable_language(C)
include(FetchContent)
FetchContent_Declare(fw_zstd_src
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR fw-no-cmake)
FetchContent_Declare(fw_lz4_src
    URL https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR fw-no-cmake)
FetchContent_MakeAvailable(fw_zstd_src fw_lz4_src)
file(GLOB FW_ZSTD_SOURCES
    ${fw_zstd_src_SOURCE_DIR}/lib/common/*.c
    ${fw_zstd_src_SOURCE_DIR}/lib/compress/*.c
    ${fw_zstd_src_SOURCE_DIR}/lib/decompress/*.c)
add_library(fw_zstd STATIC ${FW_ZSTD_SOURCES})
target_include_directories(fw_zstd PUBLIC ${fw_zstd_src_SOURCE_DIR}/lib)
target_compile_definitions(fw_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=FW_ZSTD_)
add_library(fw_lz4 STATIC ${fw_lz4_src_SOURCE_DIR}/lib/lz4.c)
target_include_directories(fw_lz4 PUBLIC ${fw_lz4_src_SOURCE_DIR}/lib)
if(MSVC)
    target_compile_options(fw_zstd PRIVATE /W0)
    target_compile_options(fw_lz4 PRIVATE /W0)
endif()

# libarchive (BSD-2-Clause) and liblzma from XZ Utils (0BSD): 7z and RAR
# archives as build sources. Pinned by hash, library code only.
include(ArchiveLibs)

include(AmprRuntime)

configure_file(src/core/BuildInfo.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/BuildInfo.h @ONLY)

add_subdirectory(src)

# core/ArchiveReader is the only user of libarchive; nothing else sees its headers.
target_link_libraries(fw_core PRIVATE fw_libarchive)

if(FW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
