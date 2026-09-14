#pragma once
#include <cstdint>
namespace cccaster::sync {
bool InstallReplayEffects();
void BeginSimulationEffects(uint32_t frame);
void BeginReplayEffects(uint32_t from, uint32_t target);
void EndReplayEffects();
void FlushSoundProbe();
} // namespace cccaster::sync
