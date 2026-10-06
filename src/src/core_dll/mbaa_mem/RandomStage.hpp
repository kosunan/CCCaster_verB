#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace cccaster::game_memory::stages {
// 手動選択は制限しない。RANDOMだけから除く利用者指定の8ステージ。
inline constexpr std::array<uint32_t, 8> RandomExcluded{32, 43, 44, 51, 54, 57, 58, 59};
inline bool RandomAllowed(uint32_t stage) {
    if (!stage || stage >= 60) return false;
    for (const auto excluded : RandomExcluded) if (stage == excluded) return false;
    return true;
}
struct RandomPool {
    std::array<uint32_t, 59> stages{};
    uint32_t count = 0;
    // 再戦では直前の番号を除く。初回・手動選択には履歴を持ち込まない。
    explicit RandomPool(std::span<const uint32_t> available, uint32_t previousStage = 0) {
        for (uint32_t stage = 1; stage < available.size() && stage < 60; ++stage)
            if (stage != previousStage && available[stage] && RandomAllowed(stage)) stages[count++] = stage;
    }
    // 2^32が候補数で割り切れない余りを棄却し、候補を等確率にする。
    bool Pick(uint32_t random, uint32_t &stage) const {
        if (!count || random < uint32_t(0 - count) % count) return false;
        stage = stages[random % count];
        return true;
    }
};
}
