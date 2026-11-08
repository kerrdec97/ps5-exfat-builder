# Single-file release (Windows). fw_onefile is the launcher stub; it becomes
# "exFAT Image Builder.exe" once fw_onefile_pack has appended the deployed
# program folder (native/packaging/make-onefile.ps1). Plain Win32, no Qt, and
# the static C runtime, so the launcher itself runs on a clean Windows 10/11
# x64 (the program it unpacks carries its own Qt and VC++ runtime DLLs).

set(FW_APP_ICON ${PROJECT_SOURCE_DIR}/src/app/windows/app.ico)
configure_file(onefile.rc.in ${CMAKE_CURRENT_BINARY_DIR}/onefile.rc @ONLY)
configure_file(onefile.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/onefile.manifest @ONLY)

set(_fw_static_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(fw_onefile_format STATIC OnefileFormat.cpp OnefileFormat.h)
target_include_directories(fw_onefile_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fw_onefile_format PUBLIC bcrypt)
set_target_properties(fw_onefile_format PROPERTIES MSVC_RUNTIME_LIBRARY ${_fw_static_crt})
fw_configure_target(fw_onefile_format)

add_executable(fw_onefile WIN32
    stub_main.cpp Runtime.cpp Runtime.h ProgressWindow.cpp ProgressWindow.h
    ${CMAKE_CURRENT_BINARY_DIR}/onefile.rc)
target_link_libraries(fw_onefile PRIVATE fw_onefile_format cabinet comctl32 user32 gdi32)
set_target_properties(fw_onefile PROPERTIES
    OUTPUT_NAME fw_onefile_stub
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY ${_fw_static_crt})
if(MSVC)
    # The fast path (program already unpacked) loads none of these.
    target_link_libraries(fw_onefile PRIVATE delayimp)
    target_link_options(fw_onefile PRIVATE /MANIFEST:NO
        /DELAYLOAD:cabinet.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:bcrypt.dll)
endif()
fw_configure_target(fw_onefile)

add_executable(fw_onefile_pack pack_main.cpp)
target_link_libraries(fw_onefile_pack PRIVATE fw_onefile_format cabinet)
target_compile_definitions(fw_onefile_pack PRIVATE FW_ONEFILE_VERSION="${PROJECT_VERSION}")
set_target_properties(fw_onefile_pack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY ${_fw_static_crt})
fw_configure_target(fw_onefile_pack)
