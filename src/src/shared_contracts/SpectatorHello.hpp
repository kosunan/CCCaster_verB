#pragma once
#include <array>
#include <cstdint>
namespace cccaster::spectator {
// TCP観戦版4: ランダム選択方法とステージ再抽選のONCE。対戦UDP版10/拡張8とは独立。
inline constexpr std::array<uint32_t, 4> Hello{0x53504343, 4, 0x01071400, 5};
}
