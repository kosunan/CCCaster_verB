#pragma once
#include <cstdint>
#include <limits>

namespace cccaster::game_build {
struct LoadedImage {
    uintptr_t base = 0;
    uint32_t size = 0;

    // 保存するのはRVA。ゲームが格納したヒープポインターには再加算しない。
    constexpr uintptr_t Resolve(uint32_t rva, uint32_t bytes = 1) const {
        if (!base || !bytes || rva >= size || bytes > size - rva ||
            base > (std::numeric_limits<uintptr_t>::max)() - rva ||
            base + rva > (std::numeric_limits<uintptr_t>::max)() - (bytes - 1)) return 0;
        return base + rva;
    }
};
}
