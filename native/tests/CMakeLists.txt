# Every tests/**/tst_*.cpp becomes one Qt Test executable registered with CTest.
# Module tests live in tests/<group>/<module>/ so specialists never edit a
# shared list.

# A small portable helper process for ProcessRunner/engine tests (prints,
# sleeps, spawns a child, exits with a given code).
add_executable(fw_test_helper support/test_helper.cpp)
fw_configure_target(fw_test_helper)

file(GLOB_RECURSE FW_TEST_SUPPORT CONFIGURE_DEPENDS support/*.h support/TestSupport.cpp)
add_library(fw_test_support STATIC ${FW_TEST_SUPPORT})
target_link_libraries(fw_test_support PUBLIC fw_registration Qt6::Test)
target_include_directories(fw_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fw_test_support PUBLIC
    FW_TEST_HELPER="$<TARGET_FILE:fw_test_helper>"
    FW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_dependencies(fw_test_support fw_test_helper)
fw_configure_target(fw_test_support)

file(GLOB_RECURSE FW_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tst_*.cpp)
foreach(test_src ${FW_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE fw_test_support)
    if(TARGET Qt6::Network)
        target_link_libraries(${test_name} PRIVATE Qt6::Network)
    endif()
    fw_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name} -o -,txt)
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
