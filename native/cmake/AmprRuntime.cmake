# zstd asset packs are served on the console by ampr-emu-ng (quer3q,
# GPL-3.0-or-later, github.com/quer3q/ampr-emu-ng), a libSceAmpr.sprx the
# build puts at fakelib/libSceAmpr.sprx of a packed game. The release build of
# the runtime is bundled next to the app as engines/ampr-emu-ng/libSceAmpr.sprx,
# pinned by SHA-256 (the release's SHA256SUMS). Its source is the tagged
# release (THIRD_PARTY_NOTICES-native.md).
#
#   FW_AMPR_RUNTIME_SPRX   a local copy of the release libSceAmpr.sprx, or
#   FW_FETCH_AMPR_RUNTIME  ON (default): download it from the release once
#                          (into the build tree) and check the hash.
# Without either, the app builds and the zstd option says the runtime is missing.

set(FW_AMPR_RUNTIME_VERSION "0.3.3")
set(FW_AMPR_RUNTIME_SHA256 "1c1051f91bc7cba19b15362b2b832114b53a4c3d6193f50c90fdf1ae5d78d268")
set(FW_AMPR_RUNTIME_URL
    "https://github.com/quer3q/ampr-emu-ng/releases/download/v${FW_AMPR_RUNTIME_VERSION}/libSceAmpr.sprx")
set(FW_AMPR_RUNTIME_SPRX "" CACHE FILEPATH "Local copy of the ampr-emu-ng ${FW_AMPR_RUNTIME_VERSION} release libSceAmpr.sprx")
option(FW_FETCH_AMPR_RUNTIME "Download the pinned ampr-emu-ng runtime when no local copy is given" ON)

set(_ampr_src "")
if(FW_AMPR_RUNTIME_SPRX)
    set(_ampr_src "${FW_AMPR_RUNTIME_SPRX}")
elseif(FW_FETCH_AMPR_RUNTIME)
    set(_ampr_src "${CMAKE_BINARY_DIR}/_deps/ampr-emu-ng/libSceAmpr-${FW_AMPR_RUNTIME_VERSION}.sprx")
    if(NOT EXISTS "${_ampr_src}")
        message(STATUS "Downloading the ampr-emu-ng runtime ${FW_AMPR_RUNTIME_VERSION}")
        # Into a temporary name and checked here: with EXPECTED_HASH a failed
        # transfer stops the configure and leaves a partial file behind.
        file(DOWNLOAD "${FW_AMPR_RUNTIME_URL}" "${_ampr_src}.tmp" STATUS _dl)
        list(GET _dl 0 _dl_code)
        set(_dl_hash "")
        if(_dl_code EQUAL 0)
            file(SHA256 "${_ampr_src}.tmp" _dl_hash)
        endif()
        if(_dl_hash STREQUAL FW_AMPR_RUNTIME_SHA256)
            file(RENAME "${_ampr_src}.tmp" "${_ampr_src}")
        else()
            file(REMOVE "${_ampr_src}.tmp")
            message(WARNING "Could not download the ampr-emu-ng runtime (${_dl}; SHA-256 \"${_dl_hash}\"). "
                            "zstd packs will be unavailable.")
            set(_ampr_src "")
        endif()
    endif()
endif()

set(FW_AMPR_RUNTIME_BUNDLE "")
if(_ampr_src)
    if(NOT EXISTS "${_ampr_src}")
        message(FATAL_ERROR "FW_AMPR_RUNTIME_SPRX does not exist: ${_ampr_src}")
    endif()
    file(SHA256 "${_ampr_src}" _ampr_hash)
    if(NOT _ampr_hash STREQUAL FW_AMPR_RUNTIME_SHA256)
        if(NOT FW_AMPR_RUNTIME_SPRX)
            file(REMOVE "${_ampr_src}") # a damaged earlier download: fetched again by the next configure
        endif()
        message(FATAL_ERROR "The ampr-emu-ng runtime does not match the pinned SHA-256 "
                            "(${_ampr_hash} instead of ${FW_AMPR_RUNTIME_SHA256}).")
    endif()
    set(FW_AMPR_RUNTIME_BUNDLE "${PROJECT_BINARY_DIR}/bin/engines/ampr-emu-ng/libSceAmpr.sprx")
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/bin/engines/ampr-emu-ng")
    configure_file("${_ampr_src}" "${FW_AMPR_RUNTIME_BUNDLE}" COPYONLY)
    file(WRITE "${PROJECT_BINARY_DIR}/bin/engines/ampr-emu-ng/VERSION.txt"
         "ampr-emu-ng ${FW_AMPR_RUNTIME_VERSION}\nsha256 ${FW_AMPR_RUNTIME_SHA256}\n"
         "source https://github.com/quer3q/ampr-emu-ng/tree/v${FW_AMPR_RUNTIME_VERSION}\nlicence GPL-3.0-or-later\n")
    message(STATUS "ampr-emu-ng runtime ${FW_AMPR_RUNTIME_VERSION}: bundled (${_ampr_src})")
else()
    # Nothing an earlier configure bundled may stay behind.
    file(REMOVE "${PROJECT_BINARY_DIR}/bin/engines/ampr-emu-ng/libSceAmpr.sprx"
                "${PROJECT_BINARY_DIR}/bin/engines/ampr-emu-ng/VERSION.txt")
    message(STATUS "ampr-emu-ng runtime: not bundled (set FW_AMPR_RUNTIME_SPRX or FW_FETCH_AMPR_RUNTIME=ON)")
endif()
