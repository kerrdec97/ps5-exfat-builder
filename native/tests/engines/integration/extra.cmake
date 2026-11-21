# Integration test extras (included by native/tests/CMakeLists.txt; here
# CMAKE_CURRENT_LIST_DIR is native/tests/engines/integration).
#
# tst_format_matrix makes its own 7z sources with libarchive's writer (never
# linked into the application) and builds FPKG packages against the stand-in
# prospero-pub-cmd of the FPKG tests (fw_fake_pubcmd, tests/engines/fpkg).
if(TARGET tst_format_matrix)
    target_link_libraries(tst_format_matrix PRIVATE fw_libarchive_write)
    target_compile_definitions(tst_format_matrix PRIVATE FW_FAKE_PUBCMD="$<TARGET_FILE:fw_fake_pubcmd>")
    add_dependencies(tst_format_matrix fw_fake_pubcmd)
    # Builds and extracts a few hundred small images: give it the time of the
    # heavy programs and keep it off the disk while they run.
    set_tests_properties(tst_format_matrix PROPERTIES TIMEOUT 1800 RESOURCE_LOCK fw_heavy_disk LABELS heavy)
endif()
