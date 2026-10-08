#include "core_dll/ui/TrainingHitboxView.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/mbaa_mem/TrainingHitboxMenu.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"

namespace cccaster::domain::ui::training_hitbox_view {
bool Draw() {
    using namespace training_hitbox;
    constexpr ImU32 colors[]{IM_COL32(255,65,85,255),IM_COL32(78,217,105,255),IM_COL32(255,222,55,255),
        IM_COL32(69,209,255,255),IM_COL32(222,103,255,255),IM_COL32(255,155,55,255)};
    if (!cccaster::game_interface::GameMem().IsPauseMenuOpen()) {
        const auto& frame=ReadFrame();
        const auto viewport=hud::CurrentViewport();
        auto* draw=ImGui::GetForegroundDrawList();
        const float sx=viewport.width/640.f,sy=viewport.height/480.f;
        auto point=[&](int32_t x,int32_t y){return ImVec2{viewport.x+(320+(x-320.f)*frame.zoom)*sx,
            viewport.y+(432+(y-432.f)*frame.zoom)*sy};};
        draw->PushClipRect({viewport.x,viewport.y},{viewport.x+viewport.width,viewport.y+viewport.height},true);
        for(const auto& box:frame.boxes) {
            const auto a=point(box.rect.x0,box.rect.y0),b=point(box.rect.x1,box.rect.y1);
            const auto color=colors[unsigned(box.kind)];
            draw->AddRectFilled(a,b,(color&0xffffff)|IM_COL32(0,0,0,28));
            draw->AddRect(a,b,color,0,0,(std::max)(1.f,(std::min)(sx,sy)));
        }
        draw->PopClipRect();
    }
    if(!Active())return false;
    const auto& state=Current();
    hud::Canvas c;
    c.Plate({70,56,500,368},hud::Gold);
    c.Text(88,70,"HITBOX",22,hud::White,4);
    c.Text(552,79,"TRAINING",10,hud::Muted,1,150,true);
    c.Text(88,104,"UP / DOWN: SELECT     LEFT: OFF     RIGHT: ON     A: TOGGLE",10);
    for(unsigned i=0;i<Count;++i) {
        const float y=133+i*33;
        if(state.selected==i)c.Plate({84,y-4,472,30},colors[i],true);
        c.Fill({94,y+4,13,13},colors[i]);
        c.Text(121,y+2,Names[i],13,hud::White,1);
        c.Text(537,y+2,state.visible[i]?"ON":"OFF",13,state.visible[i]?colors[i]:hud::Muted,1,80,true);
    }
    c.Text(88,344,"P1 / P2, partners and projectiles. Settings apply immediately.",10,hud::Muted);
    c.Text(88,361,"Frame geometry only; invulnerability and hit conditions still apply.",10,hud::Muted);
    c.Text(88,378,"Normal throws use distance checks, separate from contact boxes.",10,hud::Muted);
    c.Text(88,402,"B / ESC / START: BACK",11,hud::Gold,1);
    return true;
}
}
