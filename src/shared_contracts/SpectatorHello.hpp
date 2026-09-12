#pragma once
#include <array>
#include <cstdint>
namespace cccaster::spectator {
// TCP観戦版1、MBAACC 1.07 Rev1.4.0、対戦通信拡張5。
inline constexpr std::array<uint32_t, 4> Hello{0x53504343, 1, 0x01071400, 5};
}
