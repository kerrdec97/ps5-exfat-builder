file(GLOB_RECURSE FW_APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE FW_APP_RESOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/*.qrc)

set(FW_APP_WIN_RC "")
if(WIN32)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/windows/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/windows/app.manifest ${CMAKE_CURRENT_BINARY_DIR}/app.manifest COPYONLY)
    set(FW_APP_WIN_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

add_executable(fw_app WIN32 ${FW_APP_SOURCES} ${FW_APP_RESOURCES} ${FW_APP_WIN_RC})
set_target_properties(fw_app PROPERTIES
    OUTPUT_NAME "exFAT Image Builder"
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
target_link_libraries(fw_app PRIVATE fw_registration Qt6::Widgets Qt6::Svg Qt6::Network)
target_include_directories(fw_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(WIN32)
    target_link_libraries(fw_app PRIVATE dwmapi)
    if(MSVC)
        target_link_options(fw_app PRIVATE /MANIFEST:NO)
    endif()
endif()
fw_configure_target(fw_app)

install(TARGETS fw_app RUNTIME DESTINATION .)
