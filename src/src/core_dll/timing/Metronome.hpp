#pragma once
// 採用した相手時計の周期補正を保持する。通信スレッドからatomicで更新し、
// InputTimelineが1/60µsの小数部を蓄積して次の締切へ適用する。
// 位相差の追従はInputTimeline/PhaseFollower、補助待機は本クラスが担当する。

#include <atomic>
#include "core_dll/timing/FrameCadence.hpp"
#include "core_dll/timing/NetworkPacing.hpp"
#include <cstdint>

namespace cccaster {
namespace core {
namespace netplay {

class Metronome {
  public:
    // ─── ライフサイクル ─────────────────────────────────
    void Start();
    void Stop();
    bool IsRunning() const {
        return _running.load(std::memory_order_acquire);
    }

    // ─── ゲームスレッド精密待機 ──────────────────────────
    /// 次ティックまで Sleep+CPUスピン で精密待機する。
    /// @param skipWait true: 待機せず次ティック時刻のみ進める（キャッチアップ用）
    /// preparationTicks: 最終QPC待機へ切り替える余裕（1/60µs）。
    /// 戻り値は次フレームの絶対締切。通常経路では入力準備前に残りを待つ。
    int64_t WaitForNextTick(bool skipWait = false, int64_t preparationTicks = 0,
                            int64_t spinGuardUs = 2000);

    // 採用時計の周期補正。1/60µsの百万分率、整数µsを経由しない。
    void SetPeriodCorrectionParts(int64_t parts) { correctionParts_.store(parts,std::memory_order_release); }
    int64_t GetPeriodCorrectionParts() const { return correctionParts_.load(std::memory_order_acquire); }
    void SetFramePeriodTicks(uint32_t ticks) {
        if (timer::NetworkPacing::Valid(ticks)) framePeriodTicks_.store(ticks, std::memory_order_release);
    }
    uint32_t GetFramePeriodTicks() const { return framePeriodTicks_.load(std::memory_order_acquire); }
    int64_t GetFrameCorrectionParts() const {
        const int64_t ticks = GetFramePeriodTicks();
        return (ticks - timer::ClockFrame) * timer::ClockParts +
               GetPeriodCorrectionParts() * ticks / timer::ClockFrame;
    }

    // ─── 現在のフレーム間隔 ─────────────────────────────
    int64_t GetCurrentIntervalUs() const;

    // ─── 定数 ──────────────────────────────────────────
    static constexpr int64_t BASE_TICK_US = 16666; // 60fps 基本間隔

  private:
    static void SleepUntil(int64_t targetTicks, bool preciseSleep, int64_t spinGuardUs);

    // ─── 次ティック時刻 ────────────────────────────────
    timer::FrameCadence cadence_;

    std::atomic<int64_t> correctionParts_{0};
    std::atomic<uint32_t> framePeriodTicks_{timer::NetworkPacing::Normal};

    // ─── 状態 ──────────────────────────────────────────
    std::atomic<bool> _running{false};
};

} // namespace netplay
} // namespace core
} // namespace cccaster
