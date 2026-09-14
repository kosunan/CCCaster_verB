// ============================================================================
// InGame_Ui_View.cpp — 対戦画面 描画実装
// ============================================================================

#include "core_dll/ui/InGame_Ui_View.hpp"
#include "core_dll/ui/State_Ui_View.hpp"

namespace cccaster::domain::ui {

void InGameUiView::Draw() {
    // 対戦中は拡張ステータスバーのみ表示
    StateUiView::DrawInGameBar();
}

} // namespace cccaster::domain::ui
