# The Backport tab runs the owner's PS5 Backport GUI release in its worker
# mode (src/app/backport/BackportHost.h). The executable is a separate,
# closed-source program by the same author (github.com/kerrdec97/PS5-Backport-GUI);
# it is bundled next to the app as engines/backport/PS5BackportGUI.exe and
# pinned by SHA-256.
#
#   FW_BACKPORT_ENGINE_EXE   a local copy of the release exe (preferred), or
#   FW_FETCH_BACKPORT_ENGINE ON: download it from the official release once
#                            (into the build tree) and check the hash.
# Without either, the app builds and shows "engine missing" on the Backport tab.

set(FW_BACKPORT_ENGINE_VERSION "0.3.4")
set(FW_BACKPORT_ENGINE_SHA256 "cf8eaa884f087a78f57ba1ae32cb28231393e622a0626a42ef4f1bd00804cf42")
set(FW_BACKPORT_ENGINE_URL
    "https://github.com/kerrdec97/PS5-Backport-GUI/releases/download/early-testing-beta-0.3.4/PS5BackportGUI-0.3.4.exe")
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
    file(SHA256 "${_engine_src}" _engine_hash)
    if(NOT _engine_hash STREQUAL FW_BACKPORT_ENGINE_SHA256)
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
    install(PROGRAMS "${_engine_src}" DESTINATION engines/backport RENAME PS5BackportGUI.exe)
    message(STATUS "PS5 Backport engine ${FW_BACKPORT_ENGINE_VERSION}: bundled (${_engine_src})")
else()
    message(STATUS "PS5 Backport engine: not bundled (set FW_BACKPORT_ENGINE_EXE or FW_FETCH_BACKPORT_ENGINE=ON)")
endif()
