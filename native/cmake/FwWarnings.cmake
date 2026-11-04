# Release builds with MSVC also produce .pdb symbol files (debug information in
# the objects, /Z7, which parallel Ninja builds need; linked with /DEBUG). The
# executable is optimised exactly as before (/OPT:REF /OPT:ICF restore the
# Release defaults that /DEBUG turns off). Symbols make crash dumps from users
# and crashing tests in CI readable; they are not shipped.
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Z7>)
    add_link_options($<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Shared compiler settings for every Farewell target.
function(fw_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /bigobj)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(FW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
        if(FW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
        QT_DISABLE_DEPRECATED_BEFORE=0x060000)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
endfunction()

# Declares one engine/feature module: a static library made of every .cpp/.h in
# the calling directory (recursively). Modules register themselves in the
# FW_MODULE_TARGETS global property so the app and the tests link all of them
# without anybody editing a shared list.
function(fw_add_module name)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
    add_library(${name} STATIC ${_srcs})
    target_link_libraries(${name} PUBLIC fw_core fw_jobs ${ARG_DEPENDS})
    fw_configure_target(${name})
    set_property(GLOBAL APPEND PROPERTY FW_MODULE_TARGETS ${name})
endfunction()
