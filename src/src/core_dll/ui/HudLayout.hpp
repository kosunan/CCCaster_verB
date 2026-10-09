#pragma once
#include <algorithm>
#include <cmath>
namespace cccaster::hud {
struct Rect { float x, y, width, height; };
struct Layout {
    float x = 0, y = 0, scale = 1;
    static Layout Fit(Rect viewport) {
        const float s = (std::max)(0.01f, (std::min)(viewport.width / 640.f, viewport.height / 480.f));
        return {viewport.x + (viewport.width - 640.f * s) / 2,
                viewport.y + (viewport.height - 480.f * s) / 2, s};
    }
    float X(float value) const { return std::round(x + value * scale); }
    float Y(float value) const { return std::round(y + value * scale); }
    static constexpr Rect Left{18, 2, 258, 28}, Right{364, 2, 258, 28};
    static constexpr Rect Settings{281, 1, 78, 18};
    // 元ゲームのROUND画像はy=70〜86。DELAYはその直下へ置く。
    static constexpr Rect BattleDelay{282, 86, 76, 16};
    static constexpr Rect SelectionGuide{8, 450, 624, 26}, TrainingGuide{233, 442, 174, 38};
    static constexpr float NameSize = 22, SettingsSize = 13, GuideSize = 10;
    static Rect Player(unsigned player) {
        return player ? Right : Left;
    }
    static Rect Wins(unsigned player) {
        const auto r = Player(player);
        return {player ? r.x + 2 : r.x + r.width - 29, r.y + 3, 27, 22};
    }
    static Rect SelectionPlayer(unsigned player) { return {player ? 364.f : 0.f, 0, 276, 20}; }
};
}
