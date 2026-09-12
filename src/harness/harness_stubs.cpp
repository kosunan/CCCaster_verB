// ============================================================================
// harness_stubs.cpp — ハーネスがリンクしない層の置き換え
//
// 置き換える対象と理由:
//   HookLog          — 本来 dllmain.cpp。DLL のログファイルパス解決が要らない
//   DirectInputHook  — dinput / ConfigManager / ImGui を引き込むため
//   StateUiLogic     — OverlayRenderer(ImGui) を引き込むため
//   TimeHooks        — MinHook を引き込むため。実 QPC をそのまま返す
//   MbaaMemTrace     — 実アドレスを直接読む観測モジュール。harness には
//                      実ゲームのメモリが無いので何もしない。
//   SceneFastBoot    — seam の外に置いた3アドレス (CC_GAME_STATE_ADDR /
//                      CC_SFX_ARRAY_ADDR / CC_FORCE_GOTO_ADDR) を直接触るため。
//                      ハーネスでは FakeGame がタイムラインで画面を進めるので
//                      メニュー自動遷移そのものは検証対象にしない。
//
// DirectInputHook はスタブであると同時に、ハーネスがローカル入力を
// 注入する口でもある。SetTestInput* で与えた値がそのまま返る。
// ============================================================================

#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/hook/TimeHooks.hpp"
#ifdef _WIN32
#include "core_dll/hook/WndProcHook.hpp"
#endif
#include "core_dll/engine/SceneFastBoot.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaMemTrace.hpp"

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <string>

// ── HookLog ────────────────────────────────────────────────
// ハーネスは標準出力に出す。ログファイルは持たない。
void HookLog(const char *msg) {
    std::printf("%s\n", msg);
    std::fflush(stdout);
}

// ── DirectInputHook ────────────────────────────────────────
namespace cccaster::game_interface {

namespace {
uint32_t g_p1 = 0;
uint32_t g_p2 = 0;
} // namespace

bool DirectInputHook::Initialize(HWND) {
    return true;
}
void DirectInputHook::Shutdown() {}
void DirectInputHook::RefreshDevices() {}
void DirectInputHook::UpdateHotplug() {}
void DirectInputHook::RequestDeviceRefresh() {}
void DirectInputHook::ReloadConfigs() {}
void DirectInputHook::Poll() {}
void DirectInputHook::PollUi() {}
uint8_t DirectInputHook::GetTrainingControls() { return 0; }

uint32_t DirectInputHook::GetPlayer1Input() {
    return g_p1;
}
uint32_t DirectInputHook::GetPlayer2Input() {
    return g_p2;
}

// harness は ini もデバイスも持たないので、スロットの読み替えは起きない。
// 実 DLL 側のフォールバック（割当スロットが空ならもう一方を使う）は、
// ここでは「注入された値をそのまま返す」で十分。
uint32_t DirectInputHook::GetLocalPlayerInput(bool isHost, bool /*soloLocal*/) {
    return isHost ? g_p1 : g_p2;
}

std::vector<JoyDeviceInfo> DirectInputHook::GetConnectedDevices() {
    return {};
}
int DirectInputHook::GetActiveDeviceDirection(int &outJoyId) {
    outJoyId = -1;
    return 0;
}
std::string DirectInputHook::GetAnyInputEdge(int) {
    return {};
}
void DirectInputHook::StartMappingPlayer1(int) {}
void DirectInputHook::StartMappingPlayer2(int) {}

void DirectInputHook::SetTestModeEnabled(bool) {}
void DirectInputHook::SetTestInputP1(uint32_t input) {
    g_p1 = input;
}
void DirectInputHook::SetTestInputP2(uint32_t input) {
    g_p2 = input;
}

} // namespace cccaster::game_interface

namespace cccaster::domain::ui::score_broadcast {
void Clear() {}
void Publish(const cccaster::domain::session::SessionScoreSnapshot &) {}
}
// ── StateUiLogic ───────────────────────────────────────────
namespace cccaster::domain::ui {

namespace {
int g_delay = 0;
int g_rollback = 0;
float g_ping = 0.0f;
float g_jitter = 0.0f;
double g_fps = 0.0;
int64_t g_frameTimeUs = 0;
float g_timeOffsetMs = 0.0f;
bool g_mappingOpen = false;
} // namespace

void StateUiLogic::SetDelay(int value) {
    g_delay = value;
}
void StateUiLogic::SetRollback(int value) {
    g_rollback = value;
}
int StateUiLogic::GetDelay() {
    return g_delay;
}
int StateUiLogic::GetRollback() {
    return g_rollback;
}

void StateUiLogic::NotifyDelayChanged() {}
void StateUiLogic::NotifyRollbackChanged() {}
bool StateUiLogic::IsDelayPopupActive() {
    return false;
}
bool StateUiLogic::IsRollbackPopupActive() {
    return false;
}

void StateUiLogic::ResetUtilityMetrics(bool) { g_ping = g_jitter = 0; }
void StateUiLogic::RecordRollback(uint32_t) {}
RollbackMetricsSnapshot StateUiLogic::GetRollbackMetrics() { return {}; }
void StateUiLogic::RecordNetworkSample(float ping) { g_ping = ping; }
NetworkMetricsSnapshot StateUiLogic::GetNetworkMetrics() { return {}; }
float StateUiLogic::GetWorstPing() {
    return g_ping;
}
float StateUiLogic::GetWorstJitter() {
    return g_jitter;
}

void StateUiLogic::SetFps(double fps) {
    g_fps = fps;
}
void StateUiLogic::SetFrameTimeUs(int64_t us) {
    g_frameTimeUs = us;
}
void StateUiLogic::SetTimeOffsetMs(float ms) {
    g_timeOffsetMs = ms;
}
double StateUiLogic::GetFps() {
    return g_fps;
}
int64_t StateUiLogic::GetFrameTimeUs() {
    return g_frameTimeUs;
}
float StateUiLogic::GetTimeOffsetMs() {
    return g_timeOffsetMs;
}

void StateUiLogic::ToggleMappingWindow() {
    g_mappingOpen = !g_mappingOpen;
}
bool StateUiLogic::IsMappingWindowOpen() {
    return g_mappingOpen;
}
void StateUiLogic::CloseMappingWindow() {
    g_mappingOpen = false;
}

} // namespace cccaster::domain::ui

// ── SceneFastBoot ──────────────────────────────────────────
// SceneRunner から見た振る舞いだけ合わせる。入力の偽造もメモリ書換も行わない。
namespace cccaster::domain::scene {

namespace {
bool g_fastBootComplete = false;
}

void SceneFastBoot::Start(cccaster::public_api::IpcGameMode) {
    g_fastBootComplete = false;
}

void SceneFastBoot::Reset() {
    g_fastBootComplete = false;
}

bool SceneFastBoot::IsComplete() {
    return g_fastBootComplete;
}

bool SceneFastBoot::ProcessFrame(bool) {
    // 実装と同じ完了条件: キャラセレに到達したら完了
    if (cccaster::game_interface::GameMem().GameMode() == CC_GAME_MODE_CHARA_SELECT) {
        g_fastBootComplete = true;
        return true;
    }
    return false;
}

} // namespace cccaster::domain::scene

// ── MbaaMemTrace ───────────────────────────────────────────
namespace cccaster::game_memory {
bool MbaaMemTrace::IsEnabled() {
    return false;
}
void MbaaMemTrace::Sample(uint32_t) {}
} // namespace cccaster::game_memory

// ── TimeHooks ──────────────────────────────────────────────
// ハーネスは時間を加速しない。実 QPC をそのまま返す。
//
// Linux では TimeHooks クラスそのものが存在しない（Windows の MinHook 前提のため
// TimeHooks.hpp 全体が _WIN32 で囲まれている）。同期ロジック側は Platform 経由で
// 時刻・待機を取るようになっているので、Linux ではこのスタブ自体が不要。
#ifdef _WIN32
namespace cccaster::game_interface {
// harnessは実ウィンドウを持たない。移動中の契約はtest_window_dragで検査する。
bool WndProcHook::BlocksEscapeExit() { return false; }
}
namespace cccaster::core::hooks {

bool TimeHooks::s_initialized = false;
std::atomic<uint32_t> TimeHooks::s_multiplier{1};
std::atomic<bool> TimeHooks::s_sleepBypass{false};

void TimeHooks::Initialize() {}
void TimeHooks::Shutdown() {}
void TimeHooks::SetTimeMultiplier(uint32_t) {}
void TimeHooks::SetSleepBypass(bool) {}

void TimeHooks::RealQueryPerformanceCounter(LARGE_INTEGER *out) {
    QueryPerformanceCounter(out);
}
DWORD TimeHooks::RealGetTickCount() {
    return GetTickCount();
}
DWORD TimeHooks::RealTimeGetTime() {
    return timeGetTime();
}
void TimeHooks::RealSleep(DWORD ms) {
    Sleep(ms);
}

} // namespace cccaster::core::hooks
#endif // _WIN32
