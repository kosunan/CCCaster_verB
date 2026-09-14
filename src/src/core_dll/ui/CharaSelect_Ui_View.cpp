// ============================================================================
// CharaSelect_Ui_View.cpp — キャラセレ画面 描画実装
// ============================================================================
//
// 描画優先度（排他的に1つだけ描画）:
//   1. Controller マッピング画面 (F4)
//   2. D値変更ポップアップ (0.8秒)
//   3. R値変更ポップアップ (0.8秒)
//   4. 常時ステータスバー
// ============================================================================

#include "core_dll/ui/CharaSelect_Ui_View.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_View.hpp"

namespace cccaster::domain::ui {

void CharaSelectUiView::Draw() {
    if (StateUiLogic::IsMappingWindowOpen()) {
        ControllerUiView::Draw();
    } else {
        StateUiView::DrawCharaSelectBar();
    }
}

} // namespace cccaster::domain::ui
