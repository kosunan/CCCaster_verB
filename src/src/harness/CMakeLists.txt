# ==========================================
# harness — ゲーム無しで同期ロジックを走らせる実行ファイル
# ==========================================
# core_dll の同期・通信層をそのままリンクし、ゲームメモリだけ FakeGame に
# 差し替える。ImGui / DirectInput / MinHook を引き込む層は harness_stubs.cpp
# で置き換えるため、ここには含めない。
#
# 実行例:
#   harness.exe --host --port 7600 --out host.txt
#   harness.exe --ip 127.0.0.1 --port 7600 --local-port 7601 --out client.txt

set(CORE ${CMAKE_SOURCE_DIR}/src/core_dll)

add_executable(harness
    harness_main.cpp
    FakeGame.cpp
    harness_stubs.cpp

    # ── 検証対象（実コード）──
    ${CORE}/engine/SceneRunner.cpp
    ${CORE}/engine/SceneInputFilter.cpp
    # SceneFastBoot.cpp は harness_stubs.cpp で置換（seam 外の実アドレスを触るため）
    ${CORE}/sync/NetplaySession.cpp
    ${CORE}/sync/InputTimeline.cpp
    ${CORE}/sync/NetplayClock.cpp
    ${CORE}/network/SyncCodec.cpp
    ${CORE}/network/NetplayManager.cpp
    ${CORE}/network/PacketRouter.cpp
    ${CORE}/spectator/Transport.cpp
    ${CORE}/spectator/Playback.cpp
    ${CORE}/timing/Metronome.cpp
    ${CORE}/timing/WasapiClock.cpp
    ${CORE}/mbaa_mem/PhaseMonitor.cpp
    ${CORE}/mbaa_mem/GameMemory.cpp
    ${CORE}/common/Platform.cpp
)

target_include_directories(harness PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${asio_SOURCE_DIR}/asio/include   # UdpSocket が asio を使う
)

# harness は Windows / Linux の両方でビルドする。
# 実機用 DLL と違い 32bit 制約もないので、開発機のネイティブ環境でそのまま動く。
if(WIN32)
    # ws2_32/mswsock: UDP / ole32: WASAPI クロック（オーディオデバイスが無ければ単調時計に落ちる）
    target_link_libraries(harness PRIVATE ws2_32 mswsock ole32 winmm)
    target_link_options(harness PRIVATE -static -static-libgcc -static-libstdc++)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(harness PRIVATE Threads::Threads)
endif()

target_link_libraries(harness PRIVATE cccaster_transport)

if(WIN32)
    add_executable(probe_suspended_image probe_suspended_image.cpp)
    target_include_directories(probe_suspended_image PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_options(probe_suspended_image PRIVATE -static -static-libgcc -static-libstdc++)
endif()
