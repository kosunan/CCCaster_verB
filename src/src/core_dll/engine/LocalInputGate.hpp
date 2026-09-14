#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"

namespace cccaster::domain::scene {
// 設定用の生入力は読み続け、ゲームへ公開する値だけを遮断する。
class LocalInputGate {
  public:
    game_interface::GameInput Apply(game_interface::GameInput input, bool configuring) {
        if (configuring) {
            waitForRelease_ = true;
            return {};
        }
        // 設定画面を閉じたボタンがキャラ決定へ流れないよう、全入力の解放を待つ。
        if (waitForRelease_) {
            if (input.IsNeutral())
                waitForRelease_ = false;
            return {};
        }
        return input;
    }

  private:
    bool waitForRelease_ = false;
};
} // namespace cccaster::domain::scene
