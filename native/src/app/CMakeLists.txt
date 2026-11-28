# fw_app_lib: every UI source except main.cpp (so tests/app/ can drive real
# pages); fw_app: the executable.
file(GLOB_RECURSE FW_APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(REMOVE_ITEM FW_APP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
file(GLOB_RECURSE FW_APP_RESOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/*.qrc)

add_library(fw_app_lib STATIC ${FW_APP_SOURCES} ${FW_APP_RESOURCES})
target_link_libraries(fw_app_lib PUBLIC fw_registration Qt6::Widgets Qt6::Svg Qt6::Network)
target_include_directories(fw_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(WIN32)
    target_link_libraries(fw_app_lib PUBLIC dwmapi)
endif()
fw_configure_target(fw_app_lib)

set(FW_APP_WIN_RC "")
if(WIN32)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/windows/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/windows/app.manifest ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY) # the version comes from the project
    set(FW_APP_WIN_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

# macOS: an app bundle "exFAT Image Builder.app" in bin/, with Info.plist and
# an icon made from the PNGs in resources/app. Files shipped with the program
# (engines/, tools/, LICENSES/) belong in Contents/Resources there
# (Paths::resourceDir()); packaging/make-release-unix.sh moves them in.
set(FW_APP_MAC_ICON "")
if(APPLE)
    set(FW_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT FW_MACOS_MIN_VERSION)
        set(FW_MACOS_MIN_VERSION "13.0")
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    set(_png ${CMAKE_CURRENT_SOURCE_DIR}/resources/app)
    set(_set ${CMAKE_CURRENT_BINARY_DIR}/app.iconset)
    set(FW_APP_MAC_ICON ${CMAKE_CURRENT_BINARY_DIR}/app.icns)
    add_custom_command(OUTPUT ${FW_APP_MAC_ICON}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_set}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_set}
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-16.png ${_set}/icon_16x16.png
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-32.png ${_set}/icon_16x16@2x.png
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-32.png ${_set}/icon_32x32.png
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-64.png ${_set}/icon_32x32@2x.png
        COMMAND sips -z 128 128 ${_png}/app-256.png --out ${_set}/icon_128x128.png > /dev/null
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-256.png ${_set}/icon_128x128@2x.png
        COMMAND ${CMAKE_COMMAND} -E copy ${_png}/app-256.png ${_set}/icon_256x256.png
        COMMAND iconutil -c icns ${_set} -o ${FW_APP_MAC_ICON}
        DEPENDS ${_png}/app-16.png ${_png}/app-32.png ${_png}/app-64.png ${_png}/app-256.png
        COMMENT "Making the macOS app icon")
    set_source_files_properties(${FW_APP_MAC_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

add_executable(fw_app WIN32 MACOSX_BUNDLE main.cpp ${FW_APP_WIN_RC} ${FW_APP_MAC_ICON})
set_target_properties(fw_app PROPERTIES
    OUTPUT_NAME "exFAT Image Builder"
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
if(APPLE)
    set_target_properties(fw_app PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
endif()
target_link_libraries(fw_app PRIVATE fw_app_lib)
if(MSVC)
    target_link_options(fw_app PRIVATE /MANIFEST:NO)
endif()
fw_configure_target(fw_app)
fw_strip_pdb_path(fw_app)

# The bundled PS5 Backport engine (engines/backport/PS5BackportGUI.exe).
include(${PROJECT_SOURCE_DIR}/cmake/BackportEngine.cmake)
