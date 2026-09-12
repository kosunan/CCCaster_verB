#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include <array>
namespace cccaster::domain::scene {
// 元メニューを持たないFakeGame/harness用の従来経路。
// 実ゲームはRetrySelectionで確定項目だけを交換する。
class RematchChoice {
  public:
    struct Player {
        int cursor = 0, choice = -1;
        bool released = false;
        uint16_t buttons = 0;
        int vertical = 0;
    };
    std::array<Player, 2> players{};
    int result = -1;
    unsigned openingFrames = 0;
    void Reset() {
        *this = RematchChoice{};
    }
    void Step(game_interface::GameInput p1, game_interface::GameInput p2) {
        if (result >= 0)
            return;
        const bool opening = ++openingFrames <= 30;
        const std::array<game_interface::GameInput, 2> inputs{p1, p2};
        for (unsigned i = 0; i < 2; ++i) {
            auto &p = players[i];
            const auto in = inputs[i];
            const uint16_t confirm = in.buttons & (CC_BUTTON_A | CC_BUTTON_CONFIRM);
            const int vertical = (in.direction >= 1 && in.direction <= 3)   ? 1
                                 : (in.direction >= 7 && in.direction <= 9) ? -1
                                                                            : 0;
            // 戦闘から持ち越した押しっぱなしで再戦を選ばない。
            if (opening) {
                p.released = !confirm;
                p.buttons = confirm;
                p.vertical = vertical;
                continue;
            }
            if (!confirm)
                p.released = true;
            if (p.choice < 0) {
                if (vertical && vertical != p.vertical)
                    p.cursor = vertical > 0 ? 1 : 0;
                if (p.released && confirm && !p.buttons && !vertical)
                    p.choice = p.cursor;
            }
            p.buttons = confirm;
            p.vertical = vertical;
        }
        if (players[0].choice == 1 || players[1].choice == 1)
            result = 1;
        else if (players[0].choice == 0 && players[1].choice == 0)
            result = 0;
    }
};
inline RematchChoice rematchChoice;
} // namespace cccaster::domain::scene
