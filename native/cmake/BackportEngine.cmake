# The Backport tab runs the owner's PS5 Backport GUI in its worker mode
# (src/app/backport/BackportHost.h). The executable is a separate,
# closed-source program by the same author (github.com/kerrdec97/PS5-Backport-GUI);
# it is bundled next to the app as engines/backport/PS5BackportGUI.exe and
# pinned by SHA-256.
#
#   FW_BACKPORT_ENGINE_VERSION  which pinned release to bundle:
#                            0.3.5-test4 (default; the owner's published
#                            TESTING build, v0.3.5-test4, 17 Nov 2026) or
#                            0.3.4 (the last regular release,
#                            early-testing-beta-0.3.4). Example:
#                            cmake -DFW_BACKPORT_ENGINE_VERSION=0.3.4
#                                  -DFW_BACKPORT_ENGINE_EXE=<PS5BackportGUI-0.3.4.exe> ...
#   FW_BACKPORT_ENGINE_EXE   a local copy of that release's exe (preferred), or
#   FW_FETCH_BACKPORT_ENGINE ON: download it from the official release once
#                            (into the build tree) and check the hash.
# Without either, the app builds and shows "engine missing" on the Backport tab.
# The chosen version reaches the app as FW_BACKPORT_ENGINE_VERSION_STR (the
# one source of bp::kEngineVersion) and FW_BACKPORT_ENGINE_TESTING.
# packaging/make-release.ps1 compares the bundled file with the FIRST
# FW_BACKPORT_ENGINE_SHA256 line below, i.e. the default version.

set(FW_BACKPORT_ENGINE_VERSION "0.3.5-test4" CACHE STRING "PS5 Backport engine release to bundle (0.3.5-test4 or 0.3.4)")
set_property(CACHE FW_BACKPORT_ENGINE_VERSION PROPERTY STRINGS "0.3.5-test4" "0.3.4")
if(FW_BACKPORT_ENGINE_VERSION STREQUAL "0.3.5-test4")
    set(FW_BACKPORT_ENGINE_SHA256 "68d09ea57cdcea8676d297e01ef2ce55a270184982fbc8dbd1657cf1addde6f4")
    set(FW_BACKPORT_ENGINE_URL
        "https://github.com/kerrdec97/PS5-Backport-GUI/releases/download/v0.3.5-test4/PS5BackportGUI-0.3.5-test4.exe")
    set(FW_BACKPORT_ENGINE_REVISION "3e08eda5-19422409a6")
    set(FW_BACKPORT_ENGINE_TESTING ON)
elseif(FW_BACKPORT_ENGINE_VERSION STREQUAL "0.3.4")
    set(FW_BACKPORT_ENGINE_SHA256 "cf8eaa884f087a78f57ba1ae32cb28231393e622a0626a42ef4f1bd00804cf42")
    set(FW_BACKPORT_ENGINE_URL
        "https://github.com/kerrdec97/PS5-Backport-GUI/releases/download/early-testing-beta-0.3.4/PS5BackportGUI-0.3.4.exe")
    set(FW_BACKPORT_ENGINE_REVISION "3e08eda5-914d90065d")
    set(FW_BACKPORT_ENGINE_TESTING OFF)
else()
    message(FATAL_ERROR "FW_BACKPORT_ENGINE_VERSION must be 0.3.5-test4 or 0.3.4, not \"${FW_BACKPORT_ENGINE_VERSION}\"")
endif()
if(FW_BACKPORT_ENGINE_TESTING)
    set(_engine_testing 1)
else()
    set(_engine_testing 0)
endif()
target_compile_definitions(fw_app_lib PUBLIC
    "FW_BACKPORT_ENGINE_VERSION_STR=\"${FW_BACKPORT_ENGINE_VERSION}\""
    "FW_BACKPORT_ENGINE_REVISION_STR=\"${FW_BACKPORT_ENGINE_REVISION}\""
    FW_BACKPORT_ENGINE_TESTING=${_engine_testing})
set(FW_BACKPORT_ENGINE_EXE "" CACHE FILEPATH "Local copy of PS5BackportGUI-${FW_BACKPORT_ENGINE_VERSION}.exe to bundle")
option(FW_FETCH_BACKPORT_ENGINE "Download the pinned PS5 Backport engine release when no local copy is given" OFF)

set(_engine_src "")
if(FW_BACKPORT_ENGINE_EXE)
    set(_engine_src "${FW_BACKPORT_ENGINE_EXE}")
elseif(FW_FETCH_BACKPORT_ENGINE)
    set(_engine_src "${CMAKE_BINARY_DIR}/_deps/backport-engine/PS5BackportGUI-${FW_BACKPORT_ENGINE_VERSION}.exe")
    if(NOT EXISTS "${_engine_src}")
        message(STATUS "Downloading the PS5 Backport engine ${FW_BACKPORT_ENGINE_VERSION}")
        file(DOWNLOAD "${FW_BACKPORT_ENGINE_URL}" "${_engine_src}"
             EXPECTED_HASH SHA256=${FW_BACKPORT_ENGINE_SHA256} SHOW_PROGRESS STATUS _dl)
        list(GET _dl 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE "${_engine_src}")
            message(FATAL_ERROR "Could not download the PS5 Backport engine: ${_dl}")
        endif()
    endif()
endif()

set(FW_BACKPORT_ENGINE_BUNDLED OFF)
if(_engine_src)
    if(NOT EXISTS "${_engine_src}")
        message(FATAL_ERROR "FW_BACKPORT_ENGINE_EXE does not exist: ${_engine_src}")
    endif()
    # A local copy replaced in place re-runs configure, and with it the
    # SHA-256 check below (the copy step alone would bundle any file).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_src}")
    file(SHA256 "${_engine_src}" _engine_hash)
    if(NOT _engine_hash STREQUAL FW_BACKPORT_ENGINE_SHA256)
        if(NOT FW_BACKPORT_ENGINE_EXE)
            file(REMOVE "${_engine_src}") # a damaged earlier download: fetched again by the next configure
        endif()
        message(FATAL_ERROR "The PS5 Backport engine does not match the pinned SHA-256 "
                            "(${_engine_hash} instead of ${FW_BACKPORT_ENGINE_SHA256}).")
    endif()
    set(FW_BACKPORT_ENGINE_BUNDLED ON)
    set(_engine_dst "${PROJECT_BINARY_DIR}/bin/engines/backport/PS5BackportGUI.exe")
    add_custom_command(OUTPUT "${_engine_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/bin/engines/backport"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_engine_src}" "${_engine_dst}"
        DEPENDS "${_engine_src}"
        COMMENT "Bundling the PS5 Backport engine ${FW_BACKPORT_ENGINE_VERSION}")
    add_custom_target(fw_backport_engine ALL DEPENDS "${_engine_dst}")
    message(STATUS "PS5 Backport engine ${FW_BACKPORT_ENGINE_VERSION}: bundled (${_engine_src})")
else()
    # Nothing an earlier configure bundled may stay behind.
    file(REMOVE "${PROJECT_BINARY_DIR}/bin/engines/backport/PS5BackportGUI.exe")
    message(STATUS "PS5 Backport engine: not bundled (set FW_BACKPORT_ENGINE_EXE or FW_FETCH_BACKPORT_ENGINE=ON)")
endif()
