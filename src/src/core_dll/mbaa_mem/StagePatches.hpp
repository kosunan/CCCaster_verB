#pragma once
#include "shared_contracts/CheckedPatch.hpp"

namespace cccaster::game_memory::stages {
// MBAACC 1.07 Rev.1.4.0。旧CCCasterのenableDisabledStagesと同じ2本の除外リスト。
// 先頭の-1が終端になる。存在する背景の判定・順序・RANDOM抽選は元ゲームに任せる。
inline constexpr uint32_t TrainingExcluded[]{55, 57, 58};
inline constexpr uint32_t VersusExcluded[]{3, 4, 18, 20, 21, 23, 31, 32, 43, 44, 47, 49, 50, 51, 52, 54, 55, 57, 58};
inline constexpr uint8_t Enabled[]{0xff, 0xff, 0xff, 0xff};
inline constexpr uint8_t BossOverlayKey[]{'I','s','G','i','a','n','t','S','t','a','g','e'};
inline constexpr uint8_t BossOverlayDisabled[]{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
// BgList読込み後の両儀ステージBGM。旧版と同じ曲名とループ位置にする。
inline constexpr uint8_t RyougiMusicOriginal[]{0,0,0,0,0,0x20,0x33,0x40,'b','g','m','m','e','5','6',0};
inline constexpr uint8_t RyougiMusicFixed[]{0,0,0,0,0xaa,0xcc,0x1e,0x40,'b','g','m','m','e','5','5',0};

inline auto PostLoadSpecs() {
    std::array<patch::Spec, std::size(TrainingExcluded) + std::size(VersusExcluded) + 1> specs{};
    size_t index = 0;
    for (size_t i = 0; i < std::size(TrainingExcluded); ++i)
        specs[index++] = {"training_stages", 0x54cebc + i * 4,
            {reinterpret_cast<const uint8_t *>(&TrainingExcluded[i]), 4}, Enabled, false};
    for (size_t i = 0; i < std::size(VersusExcluded); ++i)
        specs[index++] = {"versus_stages", 0x54cf68 + i * 4,
            {reinterpret_cast<const uint8_t *>(&VersusExcluded[i]), 4}, Enabled, false};
    specs[index] = {"ryougi_stage_music", 0x7695e8, RyougiMusicOriginal, RyougiMusicFixed, false};
    return specs;
}
}
