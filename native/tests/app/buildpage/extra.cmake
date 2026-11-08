# tst_build_page (included by native/tests/CMakeLists.txt): the Build page's
# FPKG format drives the fake prospero-pub-cmd of the FPKG tests
# (tests/engines/fpkg/fake_pubcmd.cpp, target fw_fake_pubcmd).
if(TARGET tst_build_page)
    target_compile_definitions(tst_build_page PRIVATE FW_FAKE_PUBCMD="$<TARGET_FILE:fw_fake_pubcmd>")
    add_dependencies(tst_build_page fw_fake_pubcmd)
endif()
