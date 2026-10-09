#pragma once
#include <array>
#include <cstdint>

namespace cccaster::game_interface {
struct NativeImageRect { int32_t x0=0,y0=0,x1=0,y1=0; bool operator==(const NativeImageRect&) const = default; };
// 0x432E30の640x480画像の最終合成先。D3D viewport全体とは異なる。
// AUTOの比率は通常窓／ボーダーレス側で解決して渡す。
inline NativeImageRect CompositeImageRect(int width,int height,unsigned aspect,int autoWidth,int autoHeight) {
    if(width<=0 || height<=0 || width>16384 || height>16384)return {};
    if(!aspect)return {0,0,width,height};
    long double rw=4,rh=3;
    switch(aspect) {
    case 1:rw=autoWidth;rh=autoHeight;break;
    case 3:rw=16;rh=9;break;
    case 4:rw=16;rh=10;break;
    case 5:rw=5;rh=4;break;
    case 6:rw=15;rh=9;break;
    }
    if(rw<=0 || rh<=0)return {};
    int w=width,h=height;
    if(rw/rh>double(4.0/3.0))w=int(double((rh*4)/(rw*3)*width));
    else h=int(double((rw*3)/(rh*4)*height));
    const int x=(width-w)/2,y=(height-h)/2;
    return {x,y,x+w,y+h};
}
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
