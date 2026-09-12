#pragma once
#include <array>
#include <cstdint>
namespace cccaster::game_interface {
// MBAACC 1.07 Rev1.4.0: 3個の状態値 + 220Bの内部状態。
// 旧DllProcessManager::get/setRngStateと同じ範囲。ポインタは送らない。
using RngState = std::array<uint32_t, 58>;
static_assert(sizeof(RngState) == 232);
} // namespace cccaster::game_interface
