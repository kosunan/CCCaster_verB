// ============================================================================
// SceneInputFilter.cpp — 画面ごとの入力制約（実装）
//
// 【なぜ必要か】
//   キャラセレの「たまにズレる」は通信のずれではなく、同じ入力列から
//   両者が違う結果を出すことで起きる。カーソル移動と同一フレームの決定や、
//   高速連打での項目スキップは、1フレームの到着差で結果が変わる。
//   旧CCCaster はこれを入力側で潰していた（historyCheck）。
// ============================================================================

#include "core_dll/engine/SceneInputFilter.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

namespace cccaster::domain::scene {

using cccaster::game_interface::GameInput;
using cccaster::game_interface::GamePhase;
namespace Dir = cccaster::game_interface::Dir;

namespace {

/// 決定に使われるボタン
constexpr uint16_t kConfirmMask = CC_BUTTON_A | CC_BUTTON_CONFIRM;
/// キャンセルに使われるボタン
constexpr uint16_t kCancelMask = CC_BUTTON_B | CC_BUTTON_CANCEL;
/// 対戦中に押されるとメニューへ抜けてしまうボタン
constexpr uint16_t kMenuEscapeMask = CC_BUTTON_START | CC_BUTTON_FN1 | CC_BUTTON_FN2;

/// 「直近には起きていない」を表す番兵。加算し続けても溢れない大きさ。
constexpr uint32_t kLongAgo = 1000000;

struct State {
    uint16_t prevDirection = Dir::Neutral;
    uint32_t sinceDirChange = kLongAgo;
    uint32_t sinceConfirmTaken = kLongAgo;
};

State g_state;

void Age(uint32_t &counter) {
    if (counter < kLongAgo)
        ++counter;
}

} // namespace

void SceneInputFilter::Reset() {
    g_state = State{};
}

uint32_t SceneInputFilter::Apply(GamePhase phase, uint32_t input) {
    GameInput in = GameInput::Unpack(input);

    switch (phase) {
    case GamePhase::CharaSelect:
    case GamePhase::Rematch: {
        // ── カーソル移動の検出 ──
        //   ニュートラルへ戻す動きは「移動」ではないので封印しない。
        if (in.direction != g_state.prevDirection && in.direction != Dir::Neutral) {
            g_state.sinceDirChange = 0;
        }
        g_state.prevDirection = in.direction;

        // ── 移動直後の決定・キャンセルを封印 ──
        //   移動と決定が同一フレームに乗ると、到着順で結果が変わりうる。
        if (g_state.sinceDirChange < DIR_SEAL_FRAMES) {
            in.buttons &= static_cast<uint16_t>(~(kConfirmMask | kCancelMask));
        }

        // ── 決定の連打ガード ──
        //   直前に受け付けた決定から CONFIRM_GUARD_FRAMES 以内の決定は捨てる。
        //   高速連打で項目を飛ばすのを防ぐ。
        if (in.buttons & kConfirmMask) {
            if (g_state.sinceConfirmTaken < CONFIRM_GUARD_FRAMES) {
                in.buttons &= static_cast<uint16_t>(~kConfirmMask);
            } else {
                g_state.sinceConfirmTaken = 0;
            }
        }
        break;
    }

    case GamePhase::InGame:
        // 対戦中にメニューへ抜けるボタンを通さない
        in.buttons &= static_cast<uint16_t>(~kMenuEscapeMask);
        break;

    default:
        break;
    }

    Age(g_state.sinceDirChange);
    Age(g_state.sinceConfirmTaken);

    return in.Pack();
}

} // namespace cccaster::domain::scene
