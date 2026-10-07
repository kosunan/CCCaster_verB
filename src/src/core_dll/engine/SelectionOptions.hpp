#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/mbaa_mem/NativeDisplayOptions.hpp"
#include <atomic>

namespace cccaster::domain::scene::selection_options {
enum Action : unsigned { Toggle = 1, Close = 2, Up = 4, Down = 8, Left = 16, Right = 32 };
struct Result {
    bool block = false;
    int delayStep = 0;
    int animation = -1; // -1:変更なし、0:OFF、1:ON
    int hudStep = 0;
    int resolutionStep = 0;
    int fullscreen = -1;
    int nativeStep = 0;
};
// ゲームスレッドで消費するローカル入力だけを処理する。
class Menu {
    game_interface::GameInput previous_{};
    bool release_ = false;
public:
    static constexpr unsigned RowCount = 9, VisibleRows = 5;
    bool open = false;
    unsigned row = 0;
    Result Step(game_interface::GameInput input, unsigned action, bool available, bool mapping,
                bool delayEditable, bool animationOn, bool fullscreen = false) {
        const auto pressed = input.buttons & ~previous_.buttons;
        const auto direction = input.direction != previous_.direction ? input.direction : 0;
        previous_ = input;
        if (!available || mapping) {
            release_ |= open;
            open = false;
            action = 0;
        } else if ((action & Toggle) || (pressed & CC_BUTTON_START)) {
            open = !open;
            release_ = true;
            return {true};
        }
        if (open) {
            release_ = true;
            if ((action & Close) || (pressed & (CC_BUTTON_B | CC_BUTTON_CANCEL))) {
                open = false;
                return {true};
            }
            const bool up = (action & Up) || direction == 8;
            const bool down = (action & Down) || direction == 2;
            if (up != down) row = (row + (down ? 1 : RowCount - 1)) % RowCount;
            const bool left = (action & Left) || direction == 4;
            const bool right = (action & Right) || direction == 6;
            Result result{true};
            if (left != right) {
                if (row == 0 && delayEditable) result.delayStep = right ? 1 : -1;
                if (row == 1) result.animation = right ? 1 : 0;
                if (row == 2) result.hudStep = right ? 1 : -1;
                if (row == 3) result.resolutionStep = right ? 1 : -1;
                if (row == 4) result.fullscreen = right ? 1 : 0;
                if (row >= 5) result.nativeStep = right ? 1 : -1;
            }
            if (row == 1 && (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)))
                result.animation = animationOn ? 0 : 1;
            if (row == 2 && !result.hudStep && (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)))
                result.hudStep = 1;
            if (row == 3 && !result.resolutionStep && (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)))
                result.resolutionStep = 1;
            if (row == 4 && (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)))
                result.fullscreen = fullscreen ? 0 : 1;
            if (row >= 5 && !result.nativeStep && (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)))
                result.nativeStep = 1;
            return result;
        }
        // 閉じたSTART/Bをキャラ決定・キャンセルへ流さず、全解放を待つ。
        if (release_) {
            if (input.IsNeutral()) release_ = false;
            return {true};
        }
        return {};
    }
};
// WndProcは予約のみ。描画状態は通常更新のゲームスレッドで更新する。
inline std::atomic<unsigned> actions{0};
inline std::atomic<bool> active{false};
inline std::atomic<unsigned> heldKeys{0};
// Windowsの矢印・Enter・EscapeだけはWndProc操作と重複採取しない。
inline unsigned KeyMask(unsigned key) {
    if (key >= 0x25 && key <= 0x28) return 1u << (key - 0x25);
    return key == 0x0d ? 16u : key == 0x1b ? 32u : 0u;
}
inline Menu menu;
inline bool visible = false, delayEditable = false, animationOn = true;
inline int animationValue = -1, delay = 2;
inline bool displayAvailable = false, fullscreen = false;
inline int windowWidth = 0, windowHeight = 0;
inline std::array<int, 4> nativeValues{-1,-1,-1,-1};
inline void Queue(Action action) { actions.fetch_or(action); }
inline void Reset() {
    menu = {}; actions = 0; active = false; heldKeys = 0; visible = false;
    delayEditable = false; animationValue = -1;
    displayAvailable = false;
}
}
