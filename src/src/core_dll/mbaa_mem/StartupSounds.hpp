#pragma once
#include <cstdint>
namespace cccaster::game_memory::startup_sounds {
void Initialize(uint8_t mode);
void Ensure(uint32_t sound);
// 同期合流・音源長照合の前に戦闘用音源をそろえる。戦闘中のファイル読込みは増やさない。
void PrepareBattle();
bool Active();
}
