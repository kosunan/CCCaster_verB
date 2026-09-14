#pragma once
// ============================================================================
// State_Ui_Logic — 全画面共有のUI状態データ管理
// ============================================================================
//
// 【責務】
//   D/R 設定値、FPS、RTT、Jitter 等のUI表示に必要なデータを一元管理。
//   描画コードは含まない。View が参照する。
//
// 【更新元】
//   - InputHook.cpp      : D/R 変更通知
//   - NetplaySession    : FPS / フレーム時間 / RTT / Jitter / timeOffset
// ============================================================================

#include <cstdint>
#include <atomic>
#include "core_dll/ui/UtilityMetrics.hpp"

namespace cccaster::domain::ui {

class StateUiLogic {
  public:
    // --- D/R 設定 ---
    static void SetDelay(int value);
    static void SetRollback(int value);
    static int GetDelay();
    static int GetRollback();

    // --- D/R 変更演出タイマー ---
    static void NotifyDelayChanged();
    static void NotifyRollbackChanged();
    static bool IsDelayPopupActive();
    static bool IsRollbackPopupActive();

    // --- ネットワークメトリクス ---
    static void ResetUtilityMetrics(bool netplayActive);
    static void RecordRollback(uint32_t depth);
    static RollbackMetricsSnapshot GetRollbackMetrics();
    static void RecordNetworkSample(float rttMs);
    static NetworkMetricsSnapshot GetNetworkMetrics();
    static LatencyWarningSnapshot GetLatencyWarning(int delay, int rollback);
    static float GetWorstPing();
    static float GetWorstJitter();

    // --- フレーム情報 ---
    static void SetFps(double fps);
    static void SetFrameTimeUs(int64_t us);
    static void SetTimeOffsetMs(float ms);
    static double GetFps();
    static int64_t GetFrameTimeUs();
    static float GetTimeOffsetMs();

    // --- マッピングウィンドウ ---
    static void ToggleMappingWindow();
    static bool IsMappingWindowOpen();
    static void CloseMappingWindow();
    static void NotifyControllerSettingsClosed();
    static bool IsControllerConfirmationActive();

  private:
    static constexpr uint64_t POPUP_DURATION_MS = 800;

    static int s_delay;
    static int s_rollback;
    static uint64_t s_delayPopupEnd;
    static uint64_t s_rollbackPopupEnd;
    static double s_fps;
    static int64_t s_frameTimeUs;
    static float s_timeOffsetMs;
    static std::atomic<bool> s_showMapping;
    static uint64_t s_controllerConfirmationEnd;
};

} // namespace cccaster::domain::ui
