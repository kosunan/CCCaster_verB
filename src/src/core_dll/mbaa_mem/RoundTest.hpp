#pragma once
#include <cstdint>

namespace cccaster::testing {
// 検証専用。未指定・不正値は従来の600F。状態を持たず再計算でも同じFで発火する。
inline uint32_t RoundTestFrame(const char *value, uint32_t fallback = 600) {
    if (!value || !*value) return fallback;
    uint32_t frame = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9') return fallback;
        frame = frame * 10 + uint32_t(*value - '0');
        if (frame > 60000) return fallback;
    }
    return frame ? frame : fallback;
}
inline bool RoundTestDue(uint32_t frame, uint32_t threshold, uint32_t lastEpoch = 0) {
    return (!lastEpoch || frame / 65536 <= lastEpoch) && frame % 65536 >= threshold;
}
}
