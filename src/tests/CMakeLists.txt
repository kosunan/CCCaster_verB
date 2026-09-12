# ==========================================
# 単体テスト
# ==========================================
if(WIN32)
    add_executable(test_connection_hash test_connection_hash.cpp ../cli_launcher/network_wrapper/ConnectionHash.cpp)
    target_include_directories(test_connection_hash PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_connection_hash PRIVATE ws2_32)
    target_link_options(test_connection_hash PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME connection_hash COMMAND test_connection_hash)

    add_executable(relay_negotiation_probe relay_negotiation_probe.cpp
        ../cli_launcher/network_wrapper/SessionNegotiator.cpp
        ../cli_launcher/network_wrapper/ConnectionHash.cpp
        ../cli_launcher/ui/ConsoleRenderer.cpp ../cli_launcher/ConfigManager.cpp)
    target_include_directories(relay_negotiation_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${asio_SOURCE_DIR}/asio/include)
    target_link_libraries(relay_negotiation_probe PRIVATE cccaster_transport ws2_32 wsock32 wininet winmm)
    target_link_options(relay_negotiation_probe PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(WIN32)
    add_executable(test_window_drag test_window_drag.cpp ${CMAKE_SOURCE_DIR}/src/core_dll/hook/WndProcHook.cpp)
    target_include_directories(test_window_drag PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(test_window_drag PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME window_drag COMMAND test_window_drag)
    set_tests_properties(window_drag PROPERTIES TIMEOUT 10)
endif()
add_executable(test_spectator test_spectator.cpp ${CMAKE_SOURCE_DIR}/src/core_dll/spectator/Transport.cpp)
target_include_directories(test_spectator PRIVATE ${CMAKE_SOURCE_DIR}/src ${asio_SOURCE_DIR}/asio/include)
if(WIN32)
    target_link_libraries(test_spectator PRIVATE ws2_32 mswsock)
    target_link_options(test_spectator PRIVATE -static -static-libgcc -static-libstdc++)
else()
    target_link_libraries(test_spectator PRIVATE pthread)
endif()
add_test(NAME spectator COMMAND test_spectator)

foreach(utility_test hud_display utility_metrics session_score frame_advantage session_close)
    add_executable(test_${utility_test} test_${utility_test}.cpp)
    target_include_directories(test_${utility_test} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(WIN32)
        target_link_options(test_${utility_test} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ${utility_test} COMMAND test_${utility_test})
endforeach()
add_executable(test_controller_mapping test_controller_mapping.cpp)
target_include_directories(test_controller_mapping PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_controller_mapping PRIVATE -static -static-libgcc -static-libstdc++)
    target_sources(test_controller_mapping PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core_dll/ui/Controller_Ui_Logic.cpp
        ${CMAKE_SOURCE_DIR}/src/cli_launcher/ConfigManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core_dll/common/DataPaths.cpp)
    target_link_libraries(test_controller_mapping PRIVATE imgui)
    target_include_directories(test_controller_mapping PRIVATE ${imgui_SOURCE_DIR})
endif()
add_test(NAME controller_mapping COMMAND test_controller_mapping)
if(WIN32)
    add_executable(test_score_broadcast test_score_broadcast.cpp ${CMAKE_SOURCE_DIR}/src/core_dll/ui/ScoreBroadcast.cpp)
    target_include_directories(test_score_broadcast PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(test_score_broadcast PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME score_broadcast COMMAND test_score_broadcast)
endif()
add_executable(test_game_build test_game_build.cpp)
target_include_directories(test_game_build PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(test_game_build PRIVATE bcrypt)
    target_link_options(test_game_build PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME game_build COMMAND test_game_build)
# ゲーム本体・DLL・DirectInput に依存しない純粋ロジックテスト。
# 対象が要求する外部シンボルは stub_*.cpp で置換する。
#
# 実行: ctest --test-dir build --output-on-failure

# ── test_overlay は ImGui（win32/dx9 バックエンド）に依存するため Windows 専用 ──
if(WIN32)
add_executable(test_overlay
    test_overlay.cpp
    stub_controller_mapper.cpp
    stub_hooklog.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/ui/OverlayRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/ui/NetplayOverlay.cpp
)

target_include_directories(test_overlay PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ImGui は OverlayRenderer が include するため必要（ヘッダーのみ使用）
target_link_libraries(test_overlay PRIVATE imgui)
target_include_directories(test_overlay PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# テスト用マクロ定義（friend アクセス有効化）
target_compile_definitions(test_overlay PRIVATE NETPLAY_OVERLAY_TEST)

target_link_options(test_overlay PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ── 入力の符号化（依存ゼロ・ヘッダオンリー）──────────
add_executable(test_game_input
    test_game_input.cpp
)
target_include_directories(test_game_input PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── 画面ごとの入力制約 ────────────────────────────
add_executable(test_scene_input_filter
    test_scene_input_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/engine/SceneInputFilter.cpp
)
target_include_directories(test_scene_input_filter PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── 入力バッファ（依存ゼロ・ヘッダオンリー）──────────
add_executable(test_input_buffers
    test_input_buffers.cpp
)
target_include_directories(test_input_buffers PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── NetplayClock（θ推定・α補正）────────────────────
add_executable(test_netplay_clock
    test_netplay_clock.cpp
    stub_wasapi_clock.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/sync/NetplayClock.cpp
)
target_include_directories(test_netplay_clock PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── PhaseMonitor（seam 経由でゲーム無しに検証）────────
# RealGameMemory はリンクしない。実アドレスを触るコードが混ざっていれば
# リンクエラーかクラッシュで露見する。
add_executable(test_phase_monitor
    test_phase_monitor.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/mbaa_mem/PhaseMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/mbaa_mem/GameMemory.cpp
)
target_include_directories(test_phase_monitor PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(WIN32)
    foreach(t test_game_input test_input_buffers test_netplay_clock test_phase_monitor
             test_scene_input_filter)
        target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()

# ── ctest 登録 ──────────────────────────────────────
add_test(NAME game_input    COMMAND test_game_input)
add_test(NAME input_buffers COMMAND test_input_buffers)
add_test(NAME netplay_clock COMMAND test_netplay_clock)
add_test(NAME phase_monitor COMMAND test_phase_monitor)
add_test(NAME scene_input_filter COMMAND test_scene_input_filter)

# 同期パケットの実装を直接検証（UDPやゲームは起動しない）。
add_executable(test_sync_codec
    test_sync_codec.cpp
    stub_wasapi_clock.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/network/SyncCodec.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/sync/NetplayClock.cpp
)
target_include_directories(test_sync_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_sync_codec PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME sync_codec COMMAND test_sync_codec)

add_executable(test_udp_send test_udp_send.cpp)
target_link_libraries(test_udp_send PRIVATE cccaster_transport)
if(WIN32)
    target_link_options(test_udp_send PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME udp_send COMMAND test_udp_send)
set_tests_properties(udp_send PROPERTIES TIMEOUT 10)

add_executable(test_input_gate test_input_gate.cpp)
target_include_directories(test_input_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_input_gate PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME input_gate COMMAND test_input_gate)
if(WIN32)
    add_executable(test_ipc_isolation test_ipc_isolation.cpp)
    target_include_directories(test_ipc_isolation PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(test_ipc_isolation PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME ipc_isolation COMMAND test_ipc_isolation)
endif()
add_executable(test_rollback test_rollback.cpp)
target_include_directories(test_rollback PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_rollback PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME rollback COMMAND test_rollback)

add_executable(test_rematch test_rematch.cpp)
target_include_directories(test_rematch PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_rematch PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME rematch COMMAND test_rematch)

add_executable(test_input_timeline test_input_timeline.cpp stub_wasapi_clock.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/sync/InputTimeline.cpp
    ${CMAKE_SOURCE_DIR}/src/core_dll/engine/SceneInputFilter.cpp)
target_include_directories(test_input_timeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_input_timeline PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME input_timeline COMMAND test_input_timeline)

add_executable(test_settings_commands test_settings_commands.cpp)
target_include_directories(test_settings_commands PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_settings_commands PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME settings_commands COMMAND test_settings_commands)

add_executable(test_input_conversion test_input_conversion.cpp)
target_include_directories(test_input_conversion PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_input_conversion PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME input_conversion COMMAND test_input_conversion)

add_executable(test_frame_timing test_frame_timing.cpp)
target_include_directories(test_frame_timing PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_frame_timing PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME frame_timing COMMAND test_frame_timing)

add_executable(test_metronome test_metronome.cpp ../core_dll/timing/Metronome.cpp)
target_include_directories(test_metronome PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_metronome PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME metronome COMMAND test_metronome)
set_tests_properties(metronome PROPERTIES TIMEOUT 10)

add_executable(test_thread_signal test_thread_signal.cpp)
target_include_directories(test_thread_signal PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_thread_signal PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME thread_signal COMMAND test_thread_signal)
set_tests_properties(thread_signal PROPERTIES TIMEOUT 10)

add_executable(test_clock_continuity test_clock_continuity.cpp)
target_include_directories(test_clock_continuity PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
 target_link_options(test_clock_continuity PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME clock_continuity COMMAND test_clock_continuity)
add_executable(test_spin_probe test_spin_probe.cpp)
target_include_directories(test_spin_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_spin_probe PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME spin_probe COMMAND test_spin_probe)
add_executable(test_game_cpu_guard test_game_cpu_guard.cpp)
target_include_directories(test_game_cpu_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_game_cpu_guard PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME game_cpu_guard COMMAND test_game_cpu_guard)
if(WIN32)
    add_executable(test_scene_pair_merge test_scene_pair_merge.cpp)
    target_include_directories(test_scene_pair_merge PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(test_scene_pair_merge PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME scene_pair_merge COMMAND test_scene_pair_merge)
    add_executable(test_sound_prewarm test_sound_prewarm.cpp)
    target_include_directories(test_sound_prewarm PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(test_sound_prewarm PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME sound_prewarm COMMAND test_sound_prewarm)
endif()

add_executable(test_frame_bar test_frame_bar.cpp)
target_include_directories(test_frame_bar PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_frame_bar PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME frame_bar COMMAND test_frame_bar)

add_executable(test_punch_diagnostic test_punch_diagnostic.cpp)
target_include_directories(test_punch_diagnostic PRIVATE ${CMAKE_SOURCE_DIR}/src ${asio_SOURCE_DIR}/asio/include)
if(WIN32)
    target_link_libraries(test_punch_diagnostic PRIVATE ws2_32 wsock32)
    target_link_options(test_punch_diagnostic PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME punch_diagnostic COMMAND test_punch_diagnostic)
set_tests_properties(punch_diagnostic PROPERTIES TIMEOUT 12)

add_executable(test_startup_negotiation test_startup_negotiation.cpp)
target_include_directories(test_startup_negotiation PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_options(test_startup_negotiation PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME startup_negotiation COMMAND test_startup_negotiation)

if(WIN32)
    add_executable(test_driver_lock_probe test_driver_lock_probe.cpp)
    target_include_directories(test_driver_lock_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_driver_lock_probe PRIVATE minhook)
    target_link_options(test_driver_lock_probe PRIVATE -static -static-libgcc -static-libstdc++)
    add_test(NAME driver_lock_probe COMMAND test_driver_lock_probe)
    set_tests_properties(driver_lock_probe PROPERTIES TIMEOUT 10)
endif()
