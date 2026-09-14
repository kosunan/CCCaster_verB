#pragma once
#include <cstdint>
namespace cccaster::game_interface {
// 0でも入力禁止の状態がある。キャラ登場、開始前、操作可能、操作禁止を混同しない。
enum class BattleProgress { Outside, CharacterIntro, PreFight, Fighting, InputLocked, Unknown };
constexpr BattleProgress ClassifyBattle(bool inGame, uint8_t intro, bool bothLocked) {
    if (!inGame) return BattleProgress::Outside;
    switch (intro) {
    case 2: return BattleProgress::CharacterIntro;
    case 1: return BattleProgress::PreFight;
    case 0: return bothLocked ? BattleProgress::InputLocked : BattleProgress::Fighting;
    default: return BattleProgress::Unknown;
    }
}
constexpr bool AllowsRollback(BattleProgress state) {
    return state == BattleProgress::CharacterIntro || state == BattleProgress::PreFight ||
           state == BattleProgress::Fighting;
}
}
