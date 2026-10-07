#pragma once
#include <array>
#include <cstdint>

namespace cccaster::game_interface {
struct ScreenResolution {
    int width = 0, height = 0;
    bool available = false, pending = false;
};
enum class NativeDisplayOption : unsigned { CharacterFilter, ScreenFilter, AspectRatio, ViewFps, Count };
struct NativeDisplayDefinition {
    std::uint32_t offset;
    int count;
    const char* name;
    std::array<const char*, 7> values;
};
// 0x42FFA0の選択肢、0x431A10の保存先、0x432E30/0x432D30の参照を照合。
inline constexpr std::array<NativeDisplayDefinition, 4> NativeDisplayDefinitions{{
    {0x160,4,"CHARACTER_FILTER",{"OFF","EDGE","FULL","LINEAR"}},
    {0x174,2,"SCREEN_FILTER",{"OFF","ON"}},
    {0x178,7,"ASPECT_RATIO",{"NORMAL","AUTO","4:3","16:9","16:10","5:4","15:9"}},
    {0x168,2,"VIEW_FPS",{"OFF","ON"}}
}};
inline const NativeDisplayDefinition* DisplayDefinition(NativeDisplayOption option) {
    const auto index = static_cast<unsigned>(option);
    return index < NativeDisplayDefinitions.size() ? &NativeDisplayDefinitions[index] : nullptr;
}
inline int NextDisplayValue(NativeDisplayOption option, int value, int step) {
    const auto* definition = DisplayDefinition(option);
    if (!definition || value < 0 || value >= definition->count || !step) return -1;
    return (value + (step > 0 ? 1 : definition->count - 1)) % definition->count;
}
}
