// ============================================================================
// State_Ui_Logic.cpp — UI状態データ管理の実装
// ============================================================================

#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/OverlayRenderer.hpp"
#include <mutex>

namespace cccaster::domain::ui {
namespace {
RollbackMetricsWindow rollbackMetrics;
NetworkMetricsWindow networkMetrics;
LatencyWarningTracker latencyWarning;
std::mutex networkMetricsMutex;
}

// --- static 変数定義 ---
int StateUiLogic::s_delay = 0;
int StateUiLogic::s_rollback = 0;
uint64_t StateUiLogic::s_delayPopupEnd = 0;
uint64_t StateUiLogic::s_rollbackPopupEnd = 0;
double StateUiLogic::s_fps = 60.0;
int64_t StateUiLogic::s_frameTimeUs = 16666;
float StateUiLogic::s_timeOffsetMs = 0.0f;
std::atomic<bool> StateUiLogic::s_showMapping{false};
uint64_t StateUiLogic::s_controllerConfirmationEnd = 0;

// --- D/R 設定 ---
void StateUiLogic::SetDelay(int value) {
    s_delay = value;
}
void StateUiLogic::SetRollback(int value) {
    s_rollback = value;
}
int StateUiLogic::GetDelay() {
    return s_delay;
}
int StateUiLogic::GetRollback() {
    return s_rollback;
}

// --- D/R 変更演出タイマー ---
void StateUiLogic::NotifyDelayChanged() {
    s_delayPopupEnd = cccaster::overlay::OverlayRenderer::GetTimeMs() + POPUP_DURATION_MS;
}
void StateUiLogic::NotifyRollbackChanged() {
    s_rollbackPopupEnd = cccaster::overlay::OverlayRenderer::GetTimeMs() + POPUP_DURATION_MS;
}
bool StateUiLogic::IsDelayPopupActive() {
    return cccaster::overlay::OverlayRenderer::GetTimeMs() < s_delayPopupEnd;
}
bool StateUiLogic::IsRollbackPopupActive() {
    return cccaster::overlay::OverlayRenderer::GetTimeMs() < s_rollbackPopupEnd;
}

// --- ネットワークメトリクス ---
void StateUiLogic::ResetUtilityMetrics(bool netplayActive) {
    rollbackMetrics.Reset(netplayActive);
    std::lock_guard<std::mutex> lock(networkMetricsMutex);
    networkMetrics.Reset();
    latencyWarning.Reset();
}
void StateUiLogic::RecordRollback(uint32_t depth) {
    rollbackMetrics.Record(cccaster::overlay::OverlayRenderer::GetTimeMs(), depth);
}
RollbackMetricsSnapshot StateUiLogic::GetRollbackMetrics() {
    return rollbackMetrics.Read(cccaster::overlay::OverlayRenderer::GetTimeMs());
}
void StateUiLogic::RecordNetworkSample(float rttMs) {
    std::lock_guard<std::mutex> lock(networkMetricsMutex);
    networkMetrics.Record(cccaster::overlay::OverlayRenderer::GetTimeMs(), rttMs);
    latencyWarning.Record(cccaster::overlay::OverlayRenderer::GetTimeMs(), rttMs);
}
NetworkMetricsSnapshot StateUiLogic::GetNetworkMetrics() {
    // 通信側のサンプル確保・集計をゲームスレッドで待たない。
    // 取得競合中は取得不能とし、前セッション値や古い値を現在値として返さない。
    std::unique_lock<std::mutex> lock(networkMetricsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    return networkMetrics.Read(cccaster::overlay::OverlayRenderer::GetTimeMs());
}
LatencyWarningSnapshot StateUiLogic::GetLatencyWarning(int delay, int rollback) {
    std::unique_lock<std::mutex> lock(networkMetricsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    return latencyWarning.Read(cccaster::overlay::OverlayRenderer::GetTimeMs(), delay, rollback);
}
float StateUiLogic::GetWorstPing() {
    return GetNetworkMetrics().maxRttMs;
}
float StateUiLogic::GetWorstJitter() {
    return GetNetworkMetrics().jitterMs;
}

// --- フレーム情報 ---
void StateUiLogic::SetFps(double fps) {
    s_fps = fps;
}
void StateUiLogic::SetFrameTimeUs(int64_t us) {
    s_frameTimeUs = us;
}
void StateUiLogic::SetTimeOffsetMs(float ms) {
    s_timeOffsetMs = ms;
}
double StateUiLogic::GetFps() {
    return s_fps;
}
int64_t StateUiLogic::GetFrameTimeUs() {
    return s_frameTimeUs;
}
float StateUiLogic::GetTimeOffsetMs() {
    return s_timeOffsetMs;
}

// --- マッピングウィンドウ ---
void StateUiLogic::ToggleMappingWindow() {
    s_showMapping = !s_showMapping;
}
bool StateUiLogic::IsMappingWindowOpen() {
    return s_showMapping.load(std::memory_order_acquire);
}
void StateUiLogic::CloseMappingWindow() {
    s_showMapping = false;
}
void StateUiLogic::NotifyControllerSettingsClosed() {
    s_controllerConfirmationEnd = cccaster::overlay::OverlayRenderer::GetTimeMs() + 10000;
}
bool StateUiLogic::IsControllerConfirmationActive() {
    return cccaster::overlay::OverlayRenderer::GetTimeMs() < s_controllerConfirmationEnd;
}

} // namespace cccaster::domain::ui
