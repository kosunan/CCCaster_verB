#pragma once
#include "core_dll/engine/LocalInputGate.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
namespace cccaster::domain::scene {
class RetryInputGate {
    LocalInputGate openingGate;
    unsigned opening = 0, settled = 0;
  public:
    game_interface::GameInput Apply(game_interface::GameInput input) {
        input = openingGate.Apply(input, ++opening <= 30);
        const bool vertical = (input.direction >= 1 && input.direction <= 3) ||
                              (input.direction >= 7 && input.direction <= 9);
        if (vertical) settled = 0;
        else if (settled < 3) ++settled;
        // メニューのカーソルが落ち着いた後だけ決定を通す。保存・取消・終了は不可。
        input.buttons &= CC_BUTTON_A | CC_BUTTON_CONFIRM;
        if (settled < 3) input.buttons = 0;
        // 結果メニューの項目確定はゲーム側のCONFIRMビットで判定される。
        // Aだけでも操作できるよう、通常入力経路でメニュー決定へ変換する。
        if (input.buttons & CC_BUTTON_A) input.buttons |= CC_BUTTON_CONFIRM;
        return input;
    }
};
}
