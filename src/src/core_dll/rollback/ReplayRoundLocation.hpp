#pragma once
#include <cstdint>
namespace cccaster::sync {
struct ReplayRoundLocation { bool valid; uintptr_t address; };
// Rev.1.4.0のround配列は再確保される。末尾や保存時ポインタでなく論理indexから引く。
constexpr ReplayRoundLocation LocateReplayRound(uintptr_t begin, uintptr_t end, uint32_t index) {
    constexpr uintptr_t stride = 0x140;
    if ((!begin && end) || end < begin || (end - begin) % stride ||
        (end - begin) / stride > 1024 || index > (end - begin) / stride)
        return {false, 0};
    return {true, index == (end - begin) / stride ? 0 : begin + index * stride};
}
}
