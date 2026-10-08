#pragma once
#include "core_dll/mbaa_mem/NativeDisplayOptions.hpp"
#include <cstdint>

namespace cccaster::game_interface::native_resolution {
ScreenResolution Read();
bool Request(int direction);
bool Restore(int width, int height);
// DxHookから実Resetの成否と実バックバッファ寸法を受け取る。
void ResetFinished(long result, unsigned width, unsigned height);
}
