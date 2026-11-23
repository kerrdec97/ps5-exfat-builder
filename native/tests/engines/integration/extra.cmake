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

# tst_archive_stream makes 7z archives with libarchive's writer and reads the
# RAR samples of libarchive's test suite (as tst_archive_source).
if(TARGET tst_archive_stream)
    target_link_libraries(tst_archive_stream PRIVATE fw_libarchive_write)
    target_compile_definitions(tst_archive_stream PRIVATE FW_LIBARCHIVE_TEST_DATA="${FW_LIBARCHIVE_TEST_DATA}")
endif()

# tst_archive_stream_more: the PFS and zstd-pack builds that stream (#37), the same way.
if(TARGET tst_archive_stream_more)
    target_link_libraries(tst_archive_stream_more PRIVATE fw_libarchive_write)
    target_compile_definitions(tst_archive_stream_more PRIVATE FW_LIBARCHIVE_TEST_DATA="${FW_LIBARCHIVE_TEST_DATA}")
endif()
