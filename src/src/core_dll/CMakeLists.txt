# ==========================================
# In-Game Hook Library (DLL)
# ==========================================

# We assume minhook and imgui targets are already defined and populated in the root CMakeLists.txt
# so we only declare our local module here.

add_library(cccaster_hook SHARED
    # === common/ — 全層から使う土台 ===
    common/LogSink.cpp
    common/Platform.cpp
    common/DataPaths.cpp

    # === mbaa_mem/ — MBAA メモリ操作・検出 ===
    mbaa_mem/dllmain.cpp
    mbaa_mem/MbaaPatcher.cpp
    mbaa_mem/PhaseMonitor.cpp
    mbaa_mem/GameMemory.cpp
    mbaa_mem/MbaaMemTrace.cpp
    mbaa_mem/RealGameMemory.cpp
    mbaa_mem/TrainingMetrics.cpp
    mbaa_mem/CombatStress.cpp
    mbaa_mem/NetplayMenu.cpp
    mbaa_mem/ReplaySave.cpp
    mbaa_mem/StartupAssets.cpp

    # === hook/ — 外部 API フック ===
    hook/DxHook.cpp
    hook/TimeHooks.cpp
    hook/DirectInputHook.cpp
    hook/WndProcHook.cpp

    # === timing/ — 時間管理 ===
    timing/WasapiClock.cpp
    timing/Metronome.cpp

    # === engine/ — メインロジック ===
    engine/SceneRunner.cpp
    engine/GameFrameOrchestrator.cpp
    engine/SceneFastBoot.cpp
    engine/SceneInputFilter.cpp

    # === network/ — 通信 ===
    network/NetplayManager.cpp
    network/SyncCodec.cpp
    network/PacketRouter.cpp
    spectator/Transport.cpp
    spectator/Playback.cpp

    # === sync/ — 同期 ===
    sync/NetplaySession.cpp
    sync/InputTimeline.cpp
    sync/NetplayClock.cpp

    rollback/ReplayEffects.cpp
    # === ui/ — 表示 ===
    ui/OverlayRenderer.cpp
    ui/State_Ui_Logic.cpp
    ui/ScoreBroadcast.cpp
    ui/LearningOverlay.cpp
    ui/State_Ui_View.cpp
    ui/InGame_Ui_View.cpp
    ui/Rematch_Ui_View.cpp
    ui/NetplayOverlay.cpp
    ui/UIManager.cpp
    ui/CharaSelect_Ui_View.cpp
    ui/Controller_Ui_View.cpp
    ui/Controller_Ui_Logic.cpp
    ui/ControllerMapper.cpp

    # === 外部参照 ===
    ../cli_launcher/ConfigManager.cpp
)

target_include_directories(cccaster_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Link everything to the hook DLL
target_link_libraries(cccaster_hook PRIVATE minhook imgui d3d9 dwmapi dinput8 dxguid cfgmgr32 hid ws2_32 mswsock ole32 winmm bcrypt)
target_include_directories(cccaster_hook PRIVATE
    ${minhook_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${asio_SOURCE_DIR}/asio/include
    ${imgui_SOURCE_DIR}/backends
)

if(WIN32)
    # DLLも静的リンクで構成し、ランタイム非依存にする
    target_link_options(cccaster_hook PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(cccaster_hook PRIVATE cccaster_transport)
