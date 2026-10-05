enable_language(RC)
find_program(CCCASTER_POWERSHELL NAMES pwsh powershell REQUIRED)
execute_process(COMMAND "${CCCASTER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/cmake/prepare_gui_dependencies.ps1" -BuildDirectory "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE prepare_result)
if(NOT prepare_result EQUAL 0)
    message(FATAL_ERROR "GUI依存ファイルの準備に失敗しました")
endif()
set(CCCASTER_GUI_DEPS "${CMAKE_BINARY_DIR}/gui-deps")
# ハッシュ検証済みのソースのみをビルドし、実行時依存をEXEへまとめる。
set(BUILD_SHARED_LIBS OFF)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory("${CCCASTER_GUI_DEPS}/freetype-VER-2-13-3" "${CMAKE_CURRENT_BINARY_DIR}/freetype" EXCLUDE_FROM_ALL)
add_library(Freetype::Freetype ALIAS freetype)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
set(RMLUI_SOURCE "${CCCASTER_GUI_DEPS}/RmlUi-ba95ffe8bfb6370efb2cdcca927eaad4710c5413")
add_subdirectory("${RMLUI_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/rmlui" EXCLUDE_FROM_ALL)
file(GLOB EMBLEM_PRESETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/emblems/*.bmp")
set(EMBLEM_RESOURCES "")
foreach(preset IN LISTS EMBLEM_PRESETS)
    get_filename_component(code "${preset}" NAME_WE)
    string(TOUPPER "EMBLEM_${code}" resource_name)
    string(APPEND EMBLEM_RESOURCES "${resource_name} RCDATA \"${preset}\"\n")
endforeach()
configure_file(rml_resources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/rml_resources.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/rml_resources.rc" PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/launcher.rml;${CMAKE_CURRENT_SOURCE_DIR}/ui/launcher.rcss;${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest;${CCCASTER_GUI_DEPS}/NotoSansJP.ttf;${EMBLEM_PRESETS}")
add_executable(CCCaster_B_GUI WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/rml_resources.rc"
    main.cpp RmlHost.cpp RmlView.cpp Worker.cpp LauncherModel.cpp
    "${RMLUI_SOURCE}/Backends/RmlUi_Platform_Win32.cpp"
    "${RMLUI_SOURCE}/Backends/RmlUi_Renderer_DX11.cpp"
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
target_include_directories(CCCaster_B_GUI SYSTEM PRIVATE "${RMLUI_SOURCE}/Backends")
target_link_libraries(CCCaster_B_GUI PRIVATE launcher cccaster_p2p cccaster_transport RmlUi::Core d3d11 dxgi d3dcompiler imm32 shell32 shlwapi comdlg32 ws2_32 wsock32 wininet winmm windowscodecs ole32 uuid advapi32)
target_compile_definitions(CCCaster_B_GUI PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(CCCaster_B_GUI PRIVATE /W4 /utf-8)
    set_property(TARGET CCCaster_B_GUI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(CCCaster_B_GUI PRIVATE -Wall -Wextra)
    target_link_options(CCCaster_B_GUI PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ライセンスだけを別添する。フォントも描画コードもEXEに埋め込む。
set(GUI_LICENSES "${CMAKE_BINARY_DIR}/bin/gui-licenses")
file(MAKE_DIRECTORY "${GUI_LICENSES}")
configure_file("${RMLUI_SOURCE}/LICENSE.txt" "${GUI_LICENSES}/RmlUi-LICENSE.txt" COPYONLY)
configure_file("${CCCASTER_GUI_DEPS}/freetype-VER-2-13-3/docs/FTL.TXT" "${GUI_LICENSES}/FreeType-FTL.txt" COPYONLY)
configure_file("${CCCASTER_GUI_DEPS}/OFL.txt" "${GUI_LICENSES}/NotoSansJP-OFL.txt" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/gui-dependencies.json" "${GUI_LICENSES}/gui-dependencies.json" COPYONLY)
