#pragma once
#include <cstdint>

namespace cccaster::game_memory::startup_file_read {
void Initialize(uint8_t mode);
void Restore();
}
