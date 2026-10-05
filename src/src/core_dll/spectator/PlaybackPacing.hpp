#pragma once
#include "core_dll/mbaa_mem/GamePhase.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

namespace cccaster::spectator {
// MBAACC: 2=イントロ演出、1=開始直前、0=通常対戦。
// 未知の値や終了演出へ高速化を拡張しない。
constexpr bool IsIntroPlayback(game_interface::GamePhase phase, unsigned intro) {
    return phase == game_interface::GamePhase::InGame && (intro == 1 || intro == 2);
}
// revisionは終了した試合数。ラウンドepochやフレーム距離では描画を切らない。
// 一度確定入力末尾へ追いついた後は、通信の揺れで再び描画OFFにしない。
constexpr bool SkipCatchupDrawing(bool catching, uint32_t match, uint32_t latestMatch) {
    return catching && latestMatch > match && latestMatch - match >= 2;
}
// 選択確定を受信済みの観戦端末だけで使用。押下と解放を分け、イントロへ持ち込まない。
constexpr game_interface::GameInput LoadingConfirm(game_interface::GamePhase phase, uint32_t tick) {
    return phase == game_interface::GamePhase::Loading && tick % 8 == 1
        ? game_interface::GameInput{0, CC_BUTTON_A | CC_BUTTON_CONFIRM} : game_interface::GameInput{};
}
}
