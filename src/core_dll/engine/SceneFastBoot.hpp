#pragma once
// ============================================================================
// SceneFastBoot — ゲームスレッド上の高速起動（メニュー自動遷移）
//
// 【責務】
//   ゲームスレッド（SceneRunner::Step()）から毎フレーム呼ばれ、
//   タイトル画面 → キャラセレまでの遷移を自動化する。
//   HighSpeedSkip_Normal モードによるフレーム高速化と、
//   GC::WriteInput() による入力偽造でメニューを遷移する。
//
// 【旧 FastBootRunner との違い】
//   - 裏スレッド → ゲームスレッド（スレッド安全）
//   - CC_SKIP_FRAMES 直書き → SpeedFlags 統一
//   - MemoryPatcher 直書き → GC::WriteInput() 統一
// ============================================================================

#include "shared_contracts/IpcData.hpp"

namespace cccaster::domain::scene {

class SceneFastBoot {
  public:
    /// @brief FastBoot を初期化する（SceneRunner::Init() から1回呼ぶ）
    static void Start(cccaster::public_api::IpcGameMode targetMode);

    /// @brief 毎フレーム処理（phase < CharaSelect のとき SceneRunner::Step() から呼ぶ）
    /// @return true: FastBoot 完了（CharaSelect に到達）
    static bool ProcessFrame(bool isHost);

    /// @brief FastBoot が完了済みかどうか
    static bool IsComplete();

    /// @brief 状態リセット
    static void Reset();
};

} // namespace cccaster::domain::scene
