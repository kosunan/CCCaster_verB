#pragma once
#include <cstdint>
namespace cccaster::game_memory::startup_assets {
void Initialize(uint8_t mode);
void Restore(bool selectionReached=false);
bool Active();
}
