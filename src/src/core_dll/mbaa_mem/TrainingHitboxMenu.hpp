#pragma once
#include "core_dll/engine/TrainingHitbox.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include <atomic>
#include <vector>

namespace cccaster::training_hitbox {
struct Box { Rect rect; Kind kind; unsigned actor; };
struct Frame { std::vector<Box> boxes; float zoom=1; };
bool Install();
bool Active();
void Open();
void Step(game_interface::GameInput&,game_interface::GameInput&,bool configuring);
const Options& Current();
const Frame& ReadFrame();
inline std::atomic<bool> escapeRequested{false},escapeHeld{false};
}
