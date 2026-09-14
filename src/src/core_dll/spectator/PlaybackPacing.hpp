#pragma once
#include "core_dll/mbaa_mem/GamePhase.hpp"

namespace cccaster::spectator {
// MBAACC: 2=イントロ演出、1=開始直前、0=通常対戦。
// 未知の値や終了演出へ高速化を拡張しない。
constexpr bool IsIntroPlayback(game_interface::GamePhase phase, unsigned intro) {
    return phase == game_interface::GamePhase::InGame && (intro == 1 || intro == 2);
}
}
