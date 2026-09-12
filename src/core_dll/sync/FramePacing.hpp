#pragma once
#include <algorithm>
#include <cstdint>
namespace cccaster::core::sync {
struct FramePacing {
    // 正なら先行側を緩め、負なら遅れ側を少し速める。1F以内は変更しない。
    static int64_t Correction(uint64_t localToken, uint64_t peerToken, uint32_t local, uint32_t peer,
                              int64_t ageUs, int64_t rttUs) {
        const auto base = uint32_t(localToken >> 32);
        if (!base || localToken != peerToken || local < base || peer < base || ageUs < 0 || ageUs > 250000)
            return 0;
        const int64_t transit = std::clamp<int64_t>(rttUs / 2 + ageUs, 0, 100000);
        const int64_t gapUs = (int64_t(local) - peer) * 1000000 / 60 - transit;
        const int64_t error = gapUs > 16667 ? gapUs - 16667 : gapUs < -16667 ? gapUs + 16667 : 0;
        return std::clamp<int64_t>(error / 64, -500, 500);
    }
};
} // namespace cccaster::core::sync
