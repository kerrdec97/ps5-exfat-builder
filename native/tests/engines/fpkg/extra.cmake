# FPKG test helpers (included by native/tests/CMakeLists.txt; here
# CMAKE_CURRENT_LIST_DIR is native/tests/engines/fpkg).
#
# fw_fake_pubcmd: a stand-in for prospero-pub-cmd.exe / prospero-dds2png.exe
# (plain C++17, no Qt, so a copy runs from any folder). The tests copy it into
# a fake toolkit layout under their temporary folder.

add_executable(fw_fake_pubcmd ${CMAKE_CURRENT_LIST_DIR}/fake_pubcmd.cpp)
fw_configure_target(fw_fake_pubcmd)
if(WIN32)
    target_link_libraries(fw_fake_pubcmd PRIVATE shell32)
endif()

file(GLOB _fw_fpkg_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tst_*.cpp)
foreach(_src ${_fw_fpkg_tests})
    get_filename_component(_name ${_src} NAME_WE)
    if(TARGET ${_name})
        target_compile_definitions(${_name} PRIVATE
            FW_FAKE_PUBCMD="$<TARGET_FILE:fw_fake_pubcmd>"
            FW_FPKG_TEST_DATA="${CMAKE_CURRENT_LIST_DIR}/../../data/fpkg")
        add_dependencies(${_name} fw_fake_pubcmd)
    endif()
endforeach()
