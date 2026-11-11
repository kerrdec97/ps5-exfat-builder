# 7z and RAR build sources (core/ArchiveReader, GitHub #37) are read with
# libarchive (BSD-2-Clause); its LZMA/LZMA2 decoder is liblzma from XZ Utils
# (0BSD). Both come from their projects' release archives, pinned by SHA-256
# (checked against the maintainers' signatures when the versions were chosen).
#
#   fw_libarchive        the reader: the 7z, RAR and RAR5 formats only, with
#                        liblzma, the app's zlib and zstd. Linked into fw_core.
#   fw_libarchive_write  the 7z writer, for the tests that make their own
#                        archives. Never linked into the application.
#
# libarchive's own CMake project is not used: it runs some two hundred
# platform checks, registers CTest dashboards and builds programs the app
# does not need. Its sources are compiled directly with the hand-written
# configuration in cmake/libarchive/ (the mechanism libarchive provides for
# this: PLATFORM_CONFIG_H), which names exactly what the reader uses. XZ
# Utils is built by its own, small CMake project, library only.

include(FetchContent)
FetchContent_Declare(fw_xz_src
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
    URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR fw-no-cmake)
FetchContent_Declare(fw_libarchive_src
    URL https://github.com/libarchive/libarchive/releases/download/v3.8.11/libarchive-3.8.11.tar.gz
    URL_HASH SHA256=900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR fw-no-cmake)
FetchContent_MakeAvailable(fw_xz_src fw_libarchive_src)

# --- liblzma -----------------------------------------------------------------
# In a function so the switches stay out of the cache and out of the rest of
# the project. EXCLUDE_FROM_ALL: only liblzma is built, and none of XZ Utils'
# install rules reach the app's packaging.
function(fw_add_liblzma)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(XZ_NLS OFF)
    set(XZ_DOC OFF)
    set(XZ_TOOL_XZ OFF)
    set(XZ_TOOL_XZDEC OFF)
    set(XZ_TOOL_LZMADEC OFF)
    set(XZ_TOOL_LZMAINFO OFF)
    set(XZ_TOOL_SCRIPTS OFF)
    set(XZ_MICROLZMA_ENCODER OFF)
    set(XZ_MICROLZMA_DECODER OFF)
    set(XZ_LZIP_DECODER OFF)
    # Translations are off: no need to look for gettext.
    set(CMAKE_DISABLE_FIND_PACKAGE_Intl ON)
    set(CMAKE_DISABLE_FIND_PACKAGE_Gettext ON)
    add_subdirectory(${fw_xz_src_SOURCE_DIR} ${fw_xz_src_BINARY_DIR} EXCLUDE_FROM_ALL)
endfunction()
fw_add_liblzma()
# XZ Utils names its public headers only for an installed copy.
target_include_directories(liblzma INTERFACE $<BUILD_INTERFACE:${fw_xz_src_SOURCE_DIR}/src/liblzma/api>)

# --- libarchive ----------------------------------------------------------------
set(FW_LIBARCHIVE_DIR ${fw_libarchive_src_SOURCE_DIR}/libarchive)
# The fixtures of libarchive's own test suite (RAR samples the tests decode).
set(FW_LIBARCHIVE_TEST_DATA ${FW_LIBARCHIVE_DIR}/test)

set(_la_read
    archive_acl.c
    archive_blake2s_ref.c
    archive_blake2sp_ref.c
    archive_check_magic.c
    archive_crc32.c
    archive_cryptor.c
    archive_digest.c
    archive_entry.c
    archive_entry_sparse.c
    archive_entry_xattr.c
    archive_hmac.c
    archive_options.c
    archive_ppmd7.c
    archive_random.c
    archive_read.c
    archive_read_add_passphrase.c
    archive_read_set_options.c
    archive_read_support_format_7zip.c
    archive_read_support_format_rar.c
    archive_read_support_format_rar5.c
    archive_string.c
    archive_string_sprintf.c
    archive_time.c
    archive_util.c
    archive_virtual.c)
if(WIN32)
    list(APPEND _la_read archive_windows.c)
endif()
set(_la_write
    archive_rb.c
    archive_write.c
    archive_write_set_format_7zip.c
    archive_write_set_options.c)
list(TRANSFORM _la_read PREPEND ${FW_LIBARCHIVE_DIR}/)
list(TRANSFORM _la_write PREPEND ${FW_LIBARCHIVE_DIR}/)

add_library(fw_libarchive STATIC ${_la_read})
add_library(fw_libarchive_write STATIC EXCLUDE_FROM_ALL ${_la_write})
foreach(_la fw_libarchive fw_libarchive_write)
    target_include_directories(${_la} PUBLIC ${FW_LIBARCHIVE_DIR})
    target_include_directories(${_la} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/libarchive)
    target_compile_definitions(${_la}
        PUBLIC LIBARCHIVE_STATIC
        PRIVATE "PLATFORM_CONFIG_H=\"fw_libarchive_config.h\"")
    target_link_libraries(${_la} PRIVATE liblzma ZLIB::ZLIB fw_zstd)
    if(WIN32)
        # archive_random.c (names of temporary files; the tests' 7z writer
        # makes one). No other part of Windows' crypto API is used.
        target_link_libraries(${_la} PRIVATE bcrypt)
    endif()
    if(MSVC)
        target_compile_options(${_la} PRIVATE /W0)
    else()
        target_compile_options(${_la} PRIVATE -w)
    endif()
endforeach()
target_link_libraries(fw_libarchive_write PUBLIC fw_libarchive)
# Third-party code: its warnings are not the app's (as for zlib, zstd and LZ4).
if(MSVC)
    target_compile_options(liblzma PRIVATE /W0)
else()
    target_compile_options(liblzma PRIVATE -w)
endif()
