# --- core: no GUI dependency -------------------------------------------------
file(GLOB_RECURSE FW_CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp core/*.h)
add_library(fw_core STATIC ${FW_CORE_SOURCES})
target_include_directories(fw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(fw_core PUBLIC Qt6::Core Qt6::Concurrent ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(fw_core PUBLIC advapi32 shell32 ole32)
endif()
fw_configure_target(fw_core)

# --- jobs: queue, history ------------------------------------------------------
file(GLOB_RECURSE FW_JOBS_SOURCES CONFIGURE_DEPENDS jobs/*.cpp jobs/*.h)
add_library(fw_jobs STATIC ${FW_JOBS_SOURCES})
target_link_libraries(fw_jobs PUBLIC fw_core)
fw_configure_target(fw_jobs)

# --- feature modules: every sub-folder with a CMakeLists.txt -------------------
foreach(group engines connect)
    file(GLOB _children LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/${group}/*)
    list(SORT _children)
    foreach(child ${_children})
        if(IS_DIRECTORY ${child} AND EXISTS ${child}/CMakeLists.txt)
            add_subdirectory(${child})
        endif()
    endforeach()
endforeach()

get_property(FW_MODULE_TARGETS GLOBAL PROPERTY FW_MODULE_TARGETS)
add_library(fw_modules INTERFACE)
target_link_libraries(fw_modules INTERFACE fw_core fw_jobs ${FW_MODULE_TARGETS})

# --- registration: calls each module's registerModule() -----------------------
add_library(fw_registration STATIC registration/Registration.cpp registration/Registration.h)
target_link_libraries(fw_registration PUBLIC fw_modules)
fw_configure_target(fw_registration)

if(FW_BUILD_APP)
    add_subdirectory(app)
endif()
