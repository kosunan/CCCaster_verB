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
#include "core_dll/engine/SelectionPreferences.hpp"
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
        constexpr float x = 8, y = 196, width = 164;
        constexpr float footer = 33 + options::Menu::VisibleRows*34;
        c.Plate({x,y,width,footer+37},Blue,true);
        c.Text(x+9,y+8,"SELECT OPTIONS",10,Blue,1);
        char position[16];
        std::snprintf(position,sizeof(position),"%u/%u",options::menu.row+1,options::Menu::RowCount);
        c.Text(x+width-9,y+9,position,9,Muted,3,28,true);
        c.Rule(x+8,y+26,width-16);
        const unsigned first = (std::min)(options::menu.row > 2 ? options::menu.row-2 : 0u,
                                          options::Menu::RowCount-options::Menu::VisibleRows);
        for (unsigned row = first; row < first+options::Menu::VisibleRows; ++row) {
            const float top = y+33+(row-first)*34;
            const bool selected = options::menu.row == row;
            if (selected) c.Fill({x+4,top-3,width-8,31},IM_COL32(23,67,97,255));
            constexpr const char* labels[]{"INPUT DELAY", "BACKGROUND ANIM.", "HUD MODE", "RESOLUTION", "FULLSCREEN",
                "CHARACTER FILTER", "SCREEN FILTER", "ASPECT RATIO", "VIEW FPS"};
            c.Text(x+10,top,labels[row],9,selected ? Blue : White,1);
            char value[32];
            if (row >= 5) {
                const auto option = static_cast<cccaster::game_interface::NativeDisplayOption>(row-5);
                const auto* definition = cccaster::game_interface::DisplayDefinition(option);
                const int selectedValue = options::nativeValues[row-5];
                std::snprintf(value,sizeof(value),"< %s >",selectedValue < 0 ? "--" : definition->values[selectedValue]);
            }
            else if (row >= 3 && !options::displayAvailable) std::snprintf(value,sizeof(value),"--");
            else if (row == 4) std::snprintf(value,sizeof(value),"< %s >",options::fullscreen ? "ON" : "OFF");
            else if (row == 3 && !options::resolutionAvailable) std::snprintf(value,sizeof(value),"--");
            else if (row == 3) std::snprintf(value,sizeof(value),"< %dx%d >",options::renderWidth,options::renderHeight);
            else if (row == 2) std::snprintf(value,sizeof(value),"< %s >",HudDisplay::Name());
            else if (row == 1) std::snprintf(value,sizeof(value),"<  %s  >",options::animationValue < 0 ? "--" : options::animationOn ? "ON" : "OFF");
            else std::snprintf(value,sizeof(value),"<  %d F  >",options::delay);
            c.Text(x+width-10,top+13,value,11,row || options::delayEditable ? White : Muted,3,100,true);
            if (!row) {
                const auto pending = cccaster::core::sync::SettingsCommands::pending.load();
                c.Text(x+10,top+14,pending ? "SYNCING" : options::delayEditable ? "0 - 8" : "LOCKED",8,
                       pending ? Gold : Muted,3);
            } else c.Text(x+10,top+14,row == 3 ? (options::resolutionPending ? "APPLYING" : "LOCAL") :
                          row == 4 ? "BORDERLESS" : "LOCAL",8,Muted,3);
        }
        c.Rule(x+8,y+footer,width-16);
        const bool saveFailed = scene::selection_preferences::SaveFailed();
        c.Text(x+9,y+footer+8,saveFailed ? "SAVE FAILED - CHECK FILE ACCESS" : "ARROWS / D-PAD   B: BACK",
               9,saveFailed ? Gold : Muted,3,width-18);
        c.Rule(x+8,y+footer+23,width-16);
        c.Text(x+9,y+footer+28,"DELAY ADDS 0 - 8 FRAMES OF INPUT LAG",7,Muted,5,width-18);
    }
}

} // namespace cccaster::domain::ui
