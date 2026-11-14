# Every tests/**/tst_*.cpp becomes one Qt Test executable registered with CTest.
#
#  * tests/engines/<name>/tst_*.cpp and tests/connect/<name>/tst_*.cpp link only
#    that module (fw_engine_<name> / fw_connect_<name>) plus the core test
#    support, so a module's tests build even while another module is broken.
#  * every other test (tests/core, tests/app, ...) links all modules.

# A small portable helper process for ProcessRunner/engine tests.
add_executable(fw_test_helper support/test_helper.cpp)
fw_configure_target(fw_test_helper)

add_library(fw_test_support_core STATIC support/TestSupport.cpp support/TestSupport.h)
target_link_libraries(fw_test_support_core PUBLIC fw_core fw_jobs Qt6::Test)
target_include_directories(fw_test_support_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fw_test_support_core PUBLIC
    FW_TEST_HELPER="$<TARGET_FILE:fw_test_helper>"
    FW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_dependencies(fw_test_support_core fw_test_helper)
fw_configure_target(fw_test_support_core)

add_library(fw_test_support STATIC support/TestRegistry.cpp support/TestRegistry.h)
target_link_libraries(fw_test_support PUBLIC fw_test_support_core fw_registration)
fw_configure_target(fw_test_support)

file(GLOB_RECURSE FW_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tst_*.cpp)
foreach(test_src ${FW_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR} ${test_src})
    if(rel MATCHES "^app/" AND NOT FW_BUILD_APP)
        continue() # UI tests need the app library
    endif()
    if(TARGET ${test_name})
        message(FATAL_ERROR "Two test files are called ${test_name}.cpp (the second is tests/${rel}). "
                            "A test's file name is its target name, so it must be unique under native/tests.")
    endif()
    add_executable(${test_name} ${test_src})
    if(rel MATCHES "^engines/([^/]+)/")
        target_link_libraries(${test_name} PRIVATE fw_test_support_core fw_engine_${CMAKE_MATCH_1})
    elseif(rel MATCHES "^connect/([^/]+)/")
        target_link_libraries(${test_name} PRIVATE fw_test_support_core fw_connect_${CMAKE_MATCH_1})
    else()
        target_link_libraries(${test_name} PRIVATE fw_test_support)
    endif()
    if(rel MATCHES "^app/")
        target_link_libraries(${test_name} PRIVATE fw_app_lib)
    endif()
    fw_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name} -o -,txt)
    # FW_DATA_DIR and the D97BETA folders are a second line of defence: a test
    # that forgets fwtest::isolateAppData, or shows the Backport page without
    # it, then works in the build tree, never in the user's app data or in the
    # files the PS5 Backport program keeps for the user.
    # QT_FORCE_STDERR_LOGGING: without it Qt Test sends its report to the
    # debugger channel on Windows when there is no console, and CTest (and
    # Testing/Temporary/LastTest.log) would show nothing of a failure.
    # FW_NO_POWER_ACTIONS: no test may put the computer to sleep or shut it
    # down, whatever it arms (power::perform also refuses in any tst_ program).
    set(_isolated "${CMAKE_CURRENT_BINARY_DIR}/isolated/${test_name}")
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 900
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;FW_OFFLINE=1;FW_NO_POWER_ACTIONS=1;FW_AMPR_RUNTIME=${FW_AMPR_RUNTIME_BUNDLE};FW_DATA_DIR=${_isolated}/appdata;D97BETA_CONFIG_DIR=${_isolated}/backport-config;D97BETA_DATA_DIR=${_isolated}/backport-data;D97BETA_ENGINE_HOME=${_isolated}/backport-engine-home")
endforeach()

# A test program started by hand (not through CTest, which sets
# FW_AMPR_RUNTIME) finds the bundled ampr-emu-ng runtime where the app looks
# for it: engines/ampr-emu-ng/ next to the program. The copy follows the
# bundle, so a build without the runtime leaves none behind.
if(CMAKE_CONFIGURATION_TYPES)
    set(_fw_test_exe_dirs "")
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        list(APPEND _fw_test_exe_dirs "${CMAKE_CURRENT_BINARY_DIR}/${cfg}")
    endforeach()
else()
    set(_fw_test_exe_dirs "${CMAKE_CURRENT_BINARY_DIR}")
endif()
foreach(dir ${_fw_test_exe_dirs})
    if(FW_AMPR_RUNTIME_BUNDLE)
        configure_file("${FW_AMPR_RUNTIME_BUNDLE}" "${dir}/engines/ampr-emu-ng/libSceAmpr.sprx" COPYONLY)
    else()
        file(REMOVE "${dir}/engines/ampr-emu-ng/libSceAmpr.sprx")
    endif()
endforeach()

# Programs that write gigabytes or load every core never run side by side, so
# the timing of the other tests does not depend on them (ctest -L heavy lists them).
foreach(heavy tst_exfat_bench tst_exfat_roundtrip tst_pfs_benchmark tst_ufs2_roundtrip)
    if(TEST ${heavy})
        set_tests_properties(${heavy} PROPERTIES RESOURCE_LOCK fw_heavy_disk LABELS heavy)
    endif()
endforeach()
# One of these starts a stand-in process named like the PS5 Backport program,
# which the others would take for a second copy of it running.
foreach(engine_test tst_backport_engine tst_backport_page tst_pagesb_backport)
    if(TEST ${engine_test})
        set_tests_properties(${engine_test} PROPERTIES RESOURCE_LOCK fw_backport_process_name)
    endif()
endforeach()

# Module-owned extras (helper executables such as a stand-in external tool):
# tests/<group>/<module>/extra.cmake (or tests/<group>/extra.cmake) is included
# if present. Inside it, CMAKE_CURRENT_LIST_DIR is that test folder.
file(GLOB FW_TEST_EXTRAS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*/extra.cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*/extra.cmake)
foreach(extra ${FW_TEST_EXTRAS})
    include(${extra})
endforeach()
