# Single-file launcher tests (included by native/tests/CMakeLists.txt; here
# CMAKE_CURRENT_LIST_DIR is native/tests/onefile). Windows only: tst_onefile
# packs a tiny payload with fw_onefile_pack and runs the fw_onefile stub.
#
#  * fw_onefile_test_child   the packed "program": a GUI exe that prints how it
#                            was started (static CRT, runs from any folder)
#  * fw_onefile_test_console a console that runs the launcher like cmd.exe does

if(WIN32 AND TARGET fw_onefile AND TARGET tst_onefile)
    set(_fw_static_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_executable(fw_onefile_test_child WIN32 ${CMAKE_CURRENT_LIST_DIR}/onefile_test_child.cpp)
    target_link_libraries(fw_onefile_test_child PRIVATE shell32)
    set_target_properties(fw_onefile_test_child PROPERTIES MSVC_RUNTIME_LIBRARY ${_fw_static_crt})
    if(MSVC)
        target_link_options(fw_onefile_test_child PRIVATE /ENTRY:wmainCRTStartup)
    endif()
    fw_configure_target(fw_onefile_test_child)

    add_executable(fw_onefile_test_console ${CMAKE_CURRENT_LIST_DIR}/onefile_test_console.cpp)
    set_target_properties(fw_onefile_test_console PROPERTIES MSVC_RUNTIME_LIBRARY ${_fw_static_crt})
    fw_configure_target(fw_onefile_test_console)

    target_compile_definitions(tst_onefile PRIVATE
        FW_ONEFILE_STUB_PATH="$<TARGET_FILE:fw_onefile>"
        FW_ONEFILE_PACK_PATH="$<TARGET_FILE:fw_onefile_pack>"
        FW_ONEFILE_CHILD_PATH="$<TARGET_FILE:fw_onefile_test_child>"
        FW_ONEFILE_CONSOLE_PATH="$<TARGET_FILE:fw_onefile_test_console>")
    add_dependencies(tst_onefile fw_onefile fw_onefile_pack fw_onefile_test_child fw_onefile_test_console)
endif()
