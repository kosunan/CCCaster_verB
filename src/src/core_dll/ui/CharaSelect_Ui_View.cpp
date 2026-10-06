// ============================================================================
// CharaSelect_Ui_View.cpp — キャラセレ画面 描画実装
// ============================================================================
//
// 描画優先度（排他的に1つだけ描画）:
//   1. Controller マッピング画面 (F4)
//   2. 名前・D/R・接続情報と操作案内
// ============================================================================

#include "core_dll/ui/CharaSelect_Ui_View.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_View.hpp"
#include "core_dll/engine/SelectionOptions.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include <cstdio>

namespace cccaster::domain::ui {

void CharaSelectUiView::Draw() {
    if (StateUiLogic::IsMappingWindowOpen()) {
        ControllerUiView::Draw();
    } else {
        StateUiView::DrawCharaSelectBar();
        namespace options = cccaster::domain::scene::selection_options;
        if (!options::visible || !options::menu.open) return;
        using namespace cccaster::hud;
        const Canvas c;
        // 左側のポートレート下部。中央の顔一覧と下端の操作案内を避ける。
        constexpr float x = 8, y = 264, width = 164;
        c.Plate({x,y,width,160},Blue,true);
        c.Text(x+9,y+8,"SELECT OPTIONS",11,Blue,1);
        c.Rule(x+8,y+26,width-16);
        for (unsigned row = 0; row < options::Menu::RowCount; ++row) {
            const float top = y+33+row*34;
            const bool selected = options::menu.row == row;
            if (selected) c.Fill({x+4,top-3,width-8,31},IM_COL32(23,67,97,255));
            c.Text(x+10,top,row == 0 ? "INPUT DELAY" : row == 1 ? "BACKGROUND ANIM." : "HUD MODE",9,selected ? Blue : White,1);
            char value[32];
            if (row == 2) std::snprintf(value,sizeof(value),"< %s >",HudDisplay::Name());
            else if (row == 1) std::snprintf(value,sizeof(value),"<  %s  >",options::animationValue < 0 ? "--" : options::animationOn ? "ON" : "OFF");
            else std::snprintf(value,sizeof(value),"<  %d F  >",options::delay);
            c.Text(x+width-10,top+13,value,11,row || options::delayEditable ? White : Muted,3,100,true);
            if (!row) {
                const auto pending = cccaster::core::sync::SettingsCommands::pending.load();
                c.Text(x+10,top+14,pending ? "SYNCING" : options::delayEditable ? "0 - 8" : "LOCKED",8,
                       pending ? Gold : Muted,3);
            } else c.Text(x+10,top+14,"LOCAL",8,Muted,3);
        }
        c.Rule(x+8,y+135,width-16);
        c.Text(x+9,y+143,"ARROWS / D-PAD   B: BACK",9,Muted,3,width-18);
    }
}

} // namespace cccaster::domain::ui
