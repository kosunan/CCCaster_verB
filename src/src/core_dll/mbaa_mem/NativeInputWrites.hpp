#pragma once
#include "core_dll/rollback/InputWriteHistory.hpp"
namespace cccaster::game_interface::native_input_writes {
bool Configure(bool enabled);
void Begin(uint32_t frame);
sync::InputWriteHistory* History();
}
