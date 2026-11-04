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
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 900
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;FW_OFFLINE=1")
endforeach()

# Module-owned extras (helper executables such as a stand-in external tool):
# tests/<group>/<module>/extra.cmake is included if present. Inside it,
# CMAKE_CURRENT_LIST_DIR is that module's test folder.
file(GLOB FW_TEST_EXTRAS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*/extra.cmake)
foreach(extra ${FW_TEST_EXTRAS})
    include(${extra})
endforeach()
