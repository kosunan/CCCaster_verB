#pragma once
#include <cstdint>
namespace cccaster::game_memory::startup_direct_entry {
// ゲームスレッドの起動処理からのみ呼ぶ。メニュー生成前の正規遷移予約を置き換える。
bool TryTraining(uint32_t currentMode);
bool TryVersus(uint32_t currentMode, bool isHost);
}
