#include "core_dll/ui/TrainingCharacterView.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/mbaa_mem/TrainingCharacterMenu.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <d3dx9tex.h>
#include <vector>

namespace cccaster::domain::ui::training_character_view {
namespace {
struct Image {
    IDirect3DTexture9* texture = nullptr;
    unsigned width = 1, height = 1;
    bool attempted = false;
    void Release() { if (texture) texture->Release(); texture = nullptr; attempted = false; }
    void Load(IDirect3DDevice9* device, const char* name) {
        if (attempted) return;
        attempted = true;
        std::vector<uint8_t> bytes;
        const auto path = std::string(".\\grp\\c_sel_AA\\") + name;
        if (!training_character::ReadImage(path.c_str(), bytes)) return;
        D3DXIMAGE_INFO info{};
        const auto create = reinterpret_cast<decltype(&D3DXCreateTextureFromFileInMemoryEx)>(0x4DE5CC);
        const auto result = create(device, bytes.data(), bytes.size(), 0, 0, 1, 0, D3DFMT_UNKNOWN,
            D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, &info, nullptr, &texture);
        if (FAILED(result) || !texture) return;
        D3DSURFACE_DESC desc{}; texture->GetLevelDesc(0, &desc);
        width = desc.Width; height = desc.Height;
        session::DebugLog("[TrainingCharacter] IMAGE name=%s source=%u/%u texture=%u/%u", name,
                          info.Width, info.Height, width, height);
    }
    void Draw(const hud::Canvas& c, hud::Rect dest, hud::Rect source, ImU32 tint = hud::White) const {
        if (texture) c.draw->AddImage(reinterpret_cast<ImTextureID>(texture), c.At(dest.x,dest.y),
            c.At(dest.x+dest.width,dest.y+dest.height), {source.x/width, source.y/height},
            {(source.x+source.width)/width,(source.y+source.height)/height}, tint);
    }
} faces, moons;
}
void Prepare(IDirect3DDevice9* device) {
    if (!training_character::Current().open) return;
    faces.Load(device,"csel_icon00"); moons.Load(device,"csel_style00");
}
void Release() { faces.Release(); moons.Release(); }
bool Draw() {
    using namespace training_character;
    const auto& state = Current();
    if (!state.open) return false;
    hud::Canvas c;
    const auto accent = state.player ? hud::Blue : hud::Red;
    c.Fill({12,12,616,456}, IM_COL32(5,9,22,247));
    c.Text(28,22,"CHARACTER",22,hud::White,4);
    c.Text(610,30,"TRAINING",10,hud::Muted,1,100,true);
    for (unsigned side = 0; side < 2; ++side) {
        const float x = 28 + side * 300;
        c.Plate({x,54,284,24}, state.player == side ? accent : hud::Line,
                state.player == side && state.field == Field::Player);
        c.Text(x+10,58,side ? "P2  /  OPPONENT" : "P1  /  PLAYER",12,
               state.player == side ? hud::White : hud::Muted,1);
        if (state.player == side) c.Text(x+268,58,"SELECTED",9,accent,1,100,true);
    }
    for (unsigned i = 0; i < Characters.size(); ++i) {
        const float x = 38 + (i%Columns)*57, y = 88 + (i/Columns)*65;
        const bool selected = i == state.index;
        c.Plate({x-1,y-2,54,63}, selected ? accent : hud::Line,
                selected && state.field == Field::Character);
        const int icon = PortraitIndex(Characters[i]);
        if (icon >= 0) faces.Draw(c,{x+2,y,48,60},{float(icon%10*48),float(icon/10*64),48,64},
                                selected ? IM_COL32_WHITE : IM_COL32(200,210,225,255));
        if (!faces.texture) {
            const auto name = CharacterName(Characters[i]);
            c.Text(x,y+22,name.c_str(),9,hud::White,3,52);
        }
        if (i >= 31) c.Text(x+2,y+48,"BOSS",9,hud::Gold,3,48);
    }
    const auto name = CharacterName(state.choice.character);
    c.Text(30,354,name.c_str(),17,hud::White,0,430);
    c.Text(610,360,"CHOOSE CHARACTER + MOON",10,hud::Muted,1,210,true);
    constexpr const char* labels[]{"CRESCENT", "FULL", "HALF", "ECLIPSE"};
    for (unsigned moon = 0; moon < MoonCount; ++moon) {
        const float x = 28 + moon*149;
        const bool available = state.MoonAvailable(moon);
        const bool selected = moon == MoonSlot(state.choice.moon);
        c.Plate({x,380,139,42},selected ? accent : hud::Line,selected && state.field == Field::Moon);
        const int icon = MoonPortraitIndex(MoonValue(state.choice.character,moon));
        // イクリプス原画は黒い円なので、明るい台紙で輪郭を見せる。
        if (moon == 3) c.Fill({x+8,384,34,34},IM_COL32(115,130,153,255));
        moons.Draw(c,{x+9,385,32,32},{float(272+icon*32),128,32,32},
                   available ? IM_COL32_WHITE : IM_COL32(110,110,110,130));
        c.Text(x+47,available ? 394 : 385,labels[moon],10,selected ? hud::White : hud::Muted,1);
        if (!available) c.Text(x+47,401,"NO DATA",8,hud::Muted,1);
    }
    c.Plate({28,432,188,24},accent,state.field == Field::Confirm);
    c.Text(42,437,"APPLY CHARACTER",11,hud::White,1);
    if (*Error()) c.Text(228,438,Error(),10,hud::Gold,3,386);
    else c.Text(610,438,"D-PAD: SELECT    A: OK    B: BACK",10,hud::Muted,3,385,true);
    return true;
}
}
