#pragma once
#include "src/core_dll/mbaa_mem/GameInput.hpp"
#include "src/core_dll/mbaa_mem/MbaaInputDefs.hpp"

// 調査用の旧直書き方式は廃止。元のファイルは
// .ai_workspace/before_input_latency/InputInjector.hpp に保存した。
// 0x55541B/1D/1Eはゲームが変換・更新する派生状態であり、入力受付口ではない。
// このファイルは本体から参照されていない。変換例としてのみ残す。
struct ControllerState {
    bool isPressedUp = false;
    bool isPressedDown = false;
    bool isPressedLeft = false;
    bool isPressedRight = false;
    bool isPressedA = false;
    bool isPressedB = false;
    bool isPressedC = false;
    bool isPressedD = false;
    bool isPressedE = false;
};

// 入力はゲーム本来の方向・ボタン形式へ変換する。
// Push/Hold/Release、向き、Eの複合入力はゲーム本体に計算させる。
inline cccaster::game_interface::GameInput toGameInput(const ControllerState &cur) {
    using namespace cccaster::game_interface;
    const int x = int(cur.isPressedRight) - int(cur.isPressedLeft);
    const int y = int(cur.isPressedUp) - int(cur.isPressedDown);
    GameInput out{};
    out.direction = (x || y) ? static_cast<uint16_t>(5 + x + 3 * y) : Dir::Neutral;
    if (cur.isPressedA)
        out.buttons |= CC_BUTTON_A;
    if (cur.isPressedB)
        out.buttons |= CC_BUTTON_B;
    if (cur.isPressedC)
        out.buttons |= CC_BUTTON_C;
    if (cur.isPressedD)
        out.buttons |= CC_BUTTON_D;
    if (cur.isPressedE)
        out.buttons |= CC_BUTTON_E;
    return out;
}

// 物理入力を直接キャラ状態へ書くことをコンパイル時に禁止する。
// 本体ではInputTimeline→中央バッファ→ゲームスレッドのRealGameMemory::WriteInputを通す。
void injectInputToProcess(const ControllerState &, const ControllerState &) = delete;
