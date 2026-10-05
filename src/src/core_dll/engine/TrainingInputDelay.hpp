#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include <array>

namespace cccaster::domain::session {
// オフラインの通常更新ごとに採取。入力設定・メニュー・保存復元をまたぐ入力は捨てる。
class TrainingInputDelay {
  public:
    using Input = game_interface::GameInput;
    using Pair = std::array<Input, 2>;
    void Clear() { history_ = {}; cursor_ = 0; }
    Pair Apply(Input p1, Input p2, int delay, bool active) {
        if (delay < 0 || delay > public_api::NetplaySettings::MaxDelay) delay = 0;
        if (delay != delay_) { Clear(); delay_ = delay; }
        constexpr auto controls = CC_BUTTON_START | CC_BUTTON_FN1 | CC_BUTTON_FN2;
        if (!active || ((p1.buttons | p2.buttons) & controls)) {
            Clear();
            return {p1, p2};
        }
        if (!delay) return {p1, p2};
        const auto output = history_[cursor_];
        history_[cursor_] = {p1, p2};
        cursor_ = (cursor_ + 1) % unsigned(delay);
        return output;
    }
  private:
    std::array<Pair, public_api::NetplaySettings::MaxDelay> history_{};
    unsigned cursor_ = 0;
    int delay_ = -1;
};
}
