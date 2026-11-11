# tst_archive_source makes its own 7z archives with libarchive's writer, which
# only this test links, and reads the RAR and 7z samples of libarchive's test
# suite from the source archive the build fetched (cmake/ArchiveLibs.cmake).
if(TARGET tst_archive_source)
    target_link_libraries(tst_archive_source PRIVATE fw_libarchive_write)
    target_compile_definitions(tst_archive_source PRIVATE
        FW_LIBARCHIVE_TEST_DATA="${FW_LIBARCHIVE_TEST_DATA}")
endif()
