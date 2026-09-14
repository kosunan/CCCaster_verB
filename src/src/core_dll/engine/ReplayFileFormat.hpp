#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace cccaster::domain::session {
// 標準REP: 0x60 header、各roundは0x8c + 4入力列(6byte) + RNG列(4byte) + 0x90。
inline bool ValidReplayFile(std::span<const unsigned char> bytes, uint32_t expectedRounds) {
    if (bytes.size() < 0x60 || std::memcmp(bytes.data(), "MBAAReplayFile", 14)) return false;
    uint32_t rounds; std::memcpy(&rounds, bytes.data() + 0x5c, 4);
    if (!rounds || rounds != expectedRounds || rounds > 10000) return false;
    size_t offset = 0x60;
    const auto skip = [&](size_t count) {
        if (count > bytes.size() - offset) return false;
        offset += count; return true;
    };
    for (uint32_t round = 0; round < rounds; ++round) {
        if (!skip(0x8c)) return false;
        for (unsigned stream = 0; stream < 5; ++stream) {
            if (bytes.size() - offset < 4) return false;
            uint32_t count; std::memcpy(&count, bytes.data() + offset, 4); offset += 4;
            const size_t stride = stream == 4 ? 4 : 6;
            if (count > (bytes.size() - offset) / stride || !skip(size_t(count) * stride)) return false;
        }
        if (!skip(0x90)) return false;
    }
    return offset == bytes.size();
}
}
