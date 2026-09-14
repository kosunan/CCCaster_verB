enable_language(RC)
configure_file(header_image.rc.in "${CMAKE_CURRENT_BINARY_DIR}/header_image.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/header_image.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/header.jpg")
add_executable(CCCaster_B_GUI WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/header_image.rc"
    main.cpp
    ../cli_launcher/ConfigManager.cpp
    ../cli_launcher/controller/MainController.cpp
    ../cli_launcher/ui/ConsoleRenderer.cpp
    ../cli_launcher/network_wrapper/SessionNegotiator.cpp
    ../cli_launcher/network_wrapper/DualRouteNegotiator.cpp
    ../cli_launcher/network_wrapper/ConnectionHash.cpp
)
if(CCCASTER_GUI_OUTPUT_NAME STREQUAL "CCCaster_GUI" OR CCCASTER_GUI_OUTPUT_NAME STREQUAL "CCCaster_v10_GUI")
    unset(CCCASTER_GUI_OUTPUT_NAME CACHE)
endif()
set(CCCASTER_GUI_OUTPUT_NAME "CCCaster_B_GUI" CACHE STRING "GUI出力名（起動中GUIを保全する検証では別名を指定）")
set_target_properties(CCCaster_B_GUI PROPERTIES OUTPUT_NAME "${CCCASTER_GUI_OUTPUT_NAME}")
target_include_directories(CCCaster_B_GUI PRIVATE ${CMAKE_SOURCE_DIR}/src ${asio_SOURCE_DIR}/asio/include)
target_link_libraries(CCCaster_B_GUI PRIVATE launcher cccaster_transport imgui d3d9 dwmapi shell32 ws2_32 wsock32 wininet winmm windowscodecs ole32 uuid)
if(MSVC)
    target_compile_options(CCCaster_B_GUI PRIVATE /W4 /utf-8)
    set_property(TARGET CCCaster_B_GUI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(CCCaster_B_GUI PRIVATE -Wall -Wextra)
    target_link_options(CCCaster_B_GUI PRIVATE -static -static-libgcc -static-libstdc++)
endif()
