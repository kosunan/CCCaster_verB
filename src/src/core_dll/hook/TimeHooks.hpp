#pragma once
// このクラスは Windows 専用（ゲーム本体のIATだけを差し替える）。
// Linux ビルドでは中身ごと存在しない。実時間の取得・待機が欲しいだけの箇所は
// `core_dll/common/Platform.hpp` を使うこと。
#ifdef _WIN32

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace cccaster::core::hooks {

/**
 * @class TimeHooks
 * @brief ゲーム本体のタイマーAPI（QPC, GTC, TGT, Sleep）をフックし、時間の進みを操作する。
 *
 * 【機能】
 *   1. 描画OFF ＋ 超倍速 (Rollup / FastBoot用)
 *      - Sleep = 0ms即リターン
 *      - タイマー類 = 1000倍速
 *   2. 描画ON ＋ 超倍速 ＋ 高精度スリープ (FastForward用)
 *      - Sleep = 指定された高精度（WASAPI等）Sleepを適用するか、0ms化して上位が管理
 *      - タイマー類 = 1000倍速
 * 
 * CCCaster自身の安定動作のため、フック前のオリジナルのAPI（RealQPCなど）も提供する。
 */
class TimeHooks {
  public:
    static void Initialize();
    static void Shutdown();

    /// @brief 時間の進む倍率を設定する (通常: 1, 爆速: 1000 等)
    static void SetTimeMultiplier(uint32_t multiplier);

    /// @brief ゲームのSleep呼び出しを完全に無視(0ms化)するかどうか
    static void SetSleepBypass(bool bypass);

    /// @brief ゲームエンジンではなく、CCCaster本体が「真の実時間」を取得するための関数
    static void RealQueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
    static DWORD RealGetTickCount();
    static DWORD RealTimeGetTime();
    static void RealSleep(DWORD dwMilliseconds);

    static bool s_initialized;
    static std::atomic<uint32_t> s_multiplier;
    static std::atomic<bool> s_sleepBypass;
};

} // namespace cccaster::core::hooks

#endif // _WIN32
