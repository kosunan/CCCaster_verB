#pragma once
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include <array>
#include <cstring>

namespace cccaster {
// 対象版0x46F5A4..0x46F60D: +0x43は排他的上限、+0x50はptr32配列。
// 読取失敗と「矩形なし」を分ける。ポインタは更新を越えて保持しない。
template<class Reader>
void ReadTrainingAnimation(uint32_t animation, bool attackDataPresent,
                           TrainingActorDetail &detail, Reader &&read) {
    detail.stanceKnown = detail.attackBoxesKnown = detail.hurtBoxesKnown = false;
    detail.animationKnown = false;
    detail.defenseSlotCount = 0;
    detail.stance = detail.attackBoxCount = detail.hurtBoxCount = detail.canMove = 0;
    detail.stateFlags2 = 0;
    std::array<uint8_t, 0x54> bytes{};
    if (!animation || !read(animation, bytes.data(), bytes.size())) return;
    detail.animationKnown = true;
    detail.defenseSlotCount = bytes[0x42];
    const auto pointer = [&](size_t offset) {
        uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    std::array<uint8_t, 0x1C> state{};
    const uint32_t stateAddress = pointer(0x38);
    if (stateAddress && read(stateAddress, state.data(), state.size())) {
        detail.stanceKnown = true;
        detail.stance = state[0xC];
        detail.canMove = state[0x11];
        std::memcpy(&detail.stateFlags2, state.data() + 0x18, sizeof(detail.stateFlags2));
    }
    // HITBOXと同じ防御配列の1..8だけがHURT。0=PUSH、9=SHIELD、11=CLASH。
    // 件数だけではNULL穴を判別できない。読取失敗は「HURTなし」にしない。
    const size_t hurtEnd = bytes[0x42] < 9 ? bytes[0x42] : 9;
    std::array<uint32_t, 9> defense{};
    detail.hurtBoxesKnown = hurtEnd <= 1;
    if (hurtEnd > 1 && pointer(0x4C) &&
        read(pointer(0x4C), defense.data(), hurtEnd * sizeof(uint32_t))) {
        detail.hurtBoxesKnown = true;
        for (size_t i = 1; i < hurtEnd; ++i) {
            if (!defense[i]) continue;
            // 0x46E67A..84は非NULLの枠だけを採用する。存在判定に座標は不要。
            // ポインタ配列1回で数える。非NULL先が壊れていても無敵とは判定しない。
            ++detail.hurtBoxCount;
        }
    }
    const auto count = bytes[0x43];
    if (!attackDataPresent || !count) {
        detail.attackBoxesKnown = true;
        return;
    }
    std::array<uint32_t, 255> boxes{};
    const uint32_t table = pointer(0x50);
    if (!table || !read(table, boxes.data(), size_t(count) * sizeof(uint32_t))) return;
    detail.attackBoxesKnown = true;
    for (size_t i = 0; i < count; ++i)
        if (boxes[i]) ++detail.attackBoxCount;
}

// 0x470260のcanMove==1分岐と0x4704C0のガード禁止bit。
// 方向・攻撃属性を満たした際の受付資格。実際に全攻撃を防げるという意味ではない。
inline bool TrainingRecoveryGuardEligible(const TrainingActorDetail &detail,
                                         int16_t lock, uint32_t sideLock, uint8_t disable) {
    return detail.stanceKnown && detail.canMove == 1 &&
        !(detail.stateFlags2 & 0x80000000u) && lock == 0 && sideLock == 0 && disable == 0 &&
        (detail.reservedPattern < 1 || detail.reservedPattern > 9);
}
} // namespace cccaster
