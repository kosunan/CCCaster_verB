#pragma once
#include <cstdint>
#include <cstddef>

namespace cccaster::core::timer {
inline int BoundaryWorkerLimit(const char *text) {
    if (!text || !*text) return 4;
    return text[0] >= '0' && text[0] <= '4' && !text[1] ? text[0] - '0' : 0;
}
inline bool BoundaryStallEnabled(const char *text) {
    return text && text[0] == '1' && !text[1];
}
// ゲームが一つの物理コアに固定されている時だけ、別の物理コアを候補にする。
// SMTの兄弟は別時計と数えず、CPU0のコアとプロセスの許可範囲も尊重する。
inline uint32_t BoundaryCpuCandidates(uint32_t allowed, uint32_t game,
                                     const uint32_t *cores, size_t count) {
    if (!game) return 0;
    uint32_t result = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cores[i] & (game | 1u)) continue;
        const auto available = cores[i] & allowed;
        result |= available & (~available + 1);
    }
    return result;
}
}
