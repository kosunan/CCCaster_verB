// ============================================================================
// Metronome.cpp — フレームリズム生成器（実装）
//
// ゲームスレッドが WaitForNextTick() を直接呼ぶ精密待機型。
// α1 + α2 補正付き間隔で待機する。
// ============================================================================

#include "core_dll/timing/Metronome.hpp"
#include "core_dll/common/TimeScale.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/Platform.hpp"

namespace cccaster {
namespace core {
namespace netplay {

// ============================================================================
// GetCurrentIntervalUs — 現在のフレーム間隔を返す
// ============================================================================
int64_t Metronome::GetCurrentIntervalUs() const {
    // HUD/疎通診断専用。実際の締切はAdvanceCorrectedを使う。
    return (timer::ClockFrame*timer::ClockParts + GetPeriodCorrectionParts()) /
           (60*timer::ClockParts*cccaster::testing::TimeScale());
}

// ============================================================================
// Start — 次ティック時刻を初期化
// ============================================================================
void Metronome::Start() {
    if (_running.load())
        return;

    SetPeriodCorrectionParts(0);
    cadence_.ResetTicks(timer::WasapiClock::GetTimeTicks());

    _running.store(true, std::memory_order_release);

    cccaster::domain::session::DebugLog("[Metronome] Started.");
}

// ============================================================================
// Stop
// ============================================================================
void Metronome::Stop() {
    if (!_running.load())
        return;
    _running.store(false, std::memory_order_release);
    cccaster::domain::session::DebugLog("[Metronome] Stopped.");
}

// ============================================================================
// SleepUntil — 精密スリープ（Sleep + スピンウェイトのハイブリッド）
// ============================================================================
void Metronome::SleepUntil(int64_t targetUs) {
    while (true) {
        int64_t remain = targetUs - timer::WasapiClock::GetTimeTicks();
        if (remain <= 0)
            break;
        if (remain > 2000 * 60) {
            // ゲーム用フックを通さず実時間で待機する。
            cccaster::platform::RealSleepMs(1);
        } else {
            cccaster::platform::CpuRelax();
        }
    }
}

// ============================================================================
// WaitForNextTick — ゲームスレッドから呼ばれる精密待機
// ============================================================================
//
// 次ティック時刻を α補正付きで計算し、その時刻まで Sleep+CPUスピン で待機する。
// skipWait=true の場合は待機せず、次ティック時刻のみ進める（キャッチアップ用）。
//
void Metronome::WaitForNextTick(bool skipWait) {
    int64_t intervalUs = GetCurrentIntervalUs();
    const int64_t now = timer::WasapiClock::GetTimeTicks();
    if (cadence_.NextTicks() < now - intervalUs * 180)
        cadence_.ResetTicks(now);
    cadence_.AdvanceCorrected(GetPeriodCorrectionParts(), cccaster::testing::TimeScale());

    if (!skipWait) {
        SleepUntil(cadence_.NextTicks());
    }
}

} // namespace netplay
} // namespace core
} // namespace cccaster
