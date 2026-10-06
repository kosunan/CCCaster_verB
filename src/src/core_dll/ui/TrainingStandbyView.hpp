#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
namespace cccaster::domain::ui::training_standby_view {
bool Step(game_interface::GameInput input);
bool Draw();
bool Active();
bool Key(unsigned key, bool repeat);
}
