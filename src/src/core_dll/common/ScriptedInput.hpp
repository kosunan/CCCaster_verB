#pragma once
// ============================================================================
// ScriptedInput — 自動テスト用の入力列
//
// 【何のためか】
//   実機テストで人がコントローラを操作すると、両者の入力が再現できず
//   決定性の判定ができない。フレーム番号だけから決まる純関数にしておけば、
//   実機でも harness でも同じ列が流れ、記録を突き合わせられる。
//
// 【フレーム番号はネットプレイフレームを使う】
//   両者が同じフレーム番号に対して同じ値を出すので、
//   受け取った側は「相手がそのフレームに何を入れたか」を検算できる。
//
// 【有効化】
//   環境変数 CCCASTER_SCRIPT_INPUT=1。通常のプレイでは無効。
// ============================================================================

#include <cstdint>
#include <cstdlib>

#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

namespace cccaster::testing {

/// 役割ごとに位相をずらした入力列。両者がゼロを出すだけの
/// 自明な一致にならないようにしてある。
inline uint32_t ScriptedInput(uint32_t frame, bool isHost) {
    namespace Dir = cccaster::game_interface::Dir;
    const uint32_t phase = (frame + (isHost ? 0u : 7u)) % 24;

    uint16_t dir = Dir::Neutral;
    uint16_t btn = 0;
    if (phase < 4)
        dir = Dir::Right;
    else if (phase < 8)
        dir = Dir::Down;
    else if (phase < 12)
        dir = Dir::Left;
    else if (phase < 16)
        dir = Dir::Up;

    if (phase % 6 == 0)
        btn = static_cast<uint16_t>(isHost ? CC_BUTTON_A : CC_BUTTON_B);
    if (phase == 18)
        btn = CC_BUTTON_CONFIRM;

    return cccaster::game_interface::GameInput{dir, btn}.Pack();
}

/// 環境変数で有効化されているか。プロセス起動時に1回だけ判定する。
inline bool IsScriptedInputEnabled() {
    static const bool enabled = [] {
        const char *v = std::getenv("CCCASTER_SCRIPT_INPUT");
        return v && v[0] == '1';
    }();
    return enabled;
}

} // namespace cccaster::testing
