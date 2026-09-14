#pragma once
/**
 * @file FrameControl.hpp
 * @brief ゲーム制御ファサード — メモリ操作を階層化した統一API
 *
 * 【2層構成】
 *
 *   制御層 (public)   : 速度モード切替・入力書込み・終了要求
 *                       Scene は「何をしたいか」だけを指示する
 *                       例: SetModeHighSpeedSkip(), WriteInput(p1, p2)
 *
 *   プリミティブ層 (private) : MbaaAddresses.hpp 定義の生アドレスへの直接操作
 *                       例: GetInputBasePtr(), WriteP1Input(), WriteP2Input()
 *
 * 【設計思想】
 *   - Scene は FrameControl:: の関数のみを呼ぶ（メモリアドレスを直接触らない）
 *   - 速度制御は SpeedFlags（RenderSkip + TickBypass）で直接管理
 *   - フレーム待機は Metronome に委譲（本クラスは関与しない）
 *   - 同期制御は NetplaySession に完全委譲
 *   - 全メソッドは static
 *
 * 【使用例】
 *   FrameControl::SetModeHighSpeedSkip(); // FastBoot / ロールアップ用高速化
 *   FrameControl::SetModeNormalSpeed();   // 通常速度復帰
 *   FrameControl::WriteInput(p1, p2);     // ゲームメモリに入力書込み
 *
 * @see SpeedFlags         描画スキップ + ティックバイパスの2フラグ
 * @see Metronome          フレーム精密待機
 * @see MbaaAddresses.hpp  メモリアドレス定義
 */

#include "core_dll/timing/SpeedFlags.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
// windows.h は include しない。OS 依存処理は Platform 経由で呼ぶ。
#include "core_dll/common/Platform.hpp"
#include <cstdint>

namespace cccaster::domain::session {

// 前方宣言: DebugLog（循環include回避）
void DebugLog(const char *fmt, ...);

class FrameControl {
  public:
    // =====================================================================
    //  Layer 2: 束ねた制御関数（Scene から呼ばれる公開API）
    // =====================================================================

    // -------------------- 速度・進行状態制御 --------------------

    /**
     * @brief 高速スキップ (起動時, FastBoot用, ロールアップ用)
     * @details RenderSkip=ON, TickBypass=ON
     */
    static void SetModeHighSpeedSkip() {
        cccaster::core::SpeedFlags::SetHighSpeed();
    }

    /**
     * @brief 通常速度
     * @details RenderSkip=OFF, TickBypass=OFF
     */
    static void SetModeNormalSpeed() {
        cccaster::core::SpeedFlags::SetNormalSpeed();
    }

    /**
     * @brief gap に応じて RenderSkip を制御する
     * @param gap   peerLatestFrame - localWriteHead（相手との差分）
     * @details
     *   gap >= 2: 相手が先行 → RenderSkip=ON（描画スキップで高速キャッチアップ）
     *   gap <  2: 通常 → RenderSkip=OFF（描画ON）
     */
    static void SetRenderSkipByGap(int32_t gap) {
        using SF = cccaster::core::SpeedFlags;
        SF::RenderSkip().store(gap >= 2, std::memory_order_release);
    }

    // -------------------- 入力操作 --------------------

    /**
     * @brief P1/P2 の入力をゲームメモリに書き込む
     */
    static void WriteInput(cccaster::game_interface::GameInput p1, cccaster::game_interface::GameInput p2) {
        cccaster::game_interface::GameMem().WriteInput(p1, p2);
    }

    // -------------------- ゲーム終了 --------------------

    /**
     * @brief ゲームプロセス（MBAA全体）の終了を要求
     *
     * 実装は Platform.cpp 側。ここでヘッダに windows.h を持ち込むと、
     * この定義1つのために FrameControl.hpp を include する全ファイルが
     * Windows 専用になるため（harness は MatchScene / SceneRunner 経由で
     * このヘッダを読む）。
     */
    static void ExitGame() {
        cccaster::platform::TerminateSelf();
    }
};

} // namespace cccaster::domain::session
