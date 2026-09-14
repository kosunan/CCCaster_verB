#pragma once
/**
 * @file SpeedFlags.hpp
 * @brief 描画スキップ + ティックバイパスの2フラグ
 *
 * 【速度制御の全体像】
 *   1. TimeHooks が QPC/GTC/TGT/Sleep を hook し、時間を1000倍速に進める（初期化時に1回設定）
 *   2. RenderSkip=true  → DxHook::EndScene 内の ImGui 描画をスキップ
 *   3. TickBypass=true   → DLLスレッドが通信スレッドのポーリングをスキップ
 *
 *   高速化したい時: SetHighSpeed()   → 両フラグ ON
 *   通常/一時停止時: SetNormalSpeed() → 両フラグ OFF
 */

#include <atomic>

namespace cccaster::core {

struct SpeedFlags {
    /// @brief 高速モード中は true → DxHook が EndScene 内の ImGui 描画をスキップ
    static std::atomic<bool> &RenderSkip() {
        static std::atomic<bool> s_renderSkip{false};
        return s_renderSkip;
    }

    /// @brief 高速モード中は true → DLL スレッドが通信スレッドのポーリングをスキップ
    static std::atomic<bool> &TickBypass() {
        static std::atomic<bool> s_tickBypass{false};
        return s_tickBypass;
    }

    /// @brief 高速化 ON (描画スキップ + ティックバイパス)
    static void SetHighSpeed() {
        RenderSkip().store(true, std::memory_order_release);
        TickBypass().store(true, std::memory_order_release);
    }

    /// @brief 通常速度 (描画あり + ティック同期あり)
    static void SetNormalSpeed() {
        RenderSkip().store(false, std::memory_order_release);
        TickBypass().store(false, std::memory_order_release);
    }
};

} // namespace cccaster::core
