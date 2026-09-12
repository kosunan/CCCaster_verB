#pragma once
#include "core_dll/ui/HudDisplay.hpp"
// ============================================================================
// State_Ui_View — 常時表示ステータスバー描画
// ============================================================================
//
// 【責務】
//   キャラセレ・対戦画面で共通表示されるステータスバーの描画。
//   State_Ui_Logic からデータを取得して ImGui で描画する。
// ============================================================================

namespace cccaster::domain::ui {

class StateUiView {
  public:
    static void CycleDisplayMode() { HudDisplay::Cycle(); }
    static HudDisplayMode GetDisplayMode() { return HudDisplay::Get(); }
    /// @brief キャラセレ用の常時ステータスバー描画
    /// 1行目: NETWORK / FRAME、2行目: SETTINGS / 操作キー
    static void DrawCharaSelectBar();

    /// @brief 対戦中用HUD（上端に名称/勝数、中央下端に小型通信情報）
    static void DrawInGameBar();
    static void DrawRematchBar();

    /// @brief D値変更時の大フォントポップアップ
    static void DrawDelayPopup();

    /// @brief R値変更時の大フォントポップアップ
    static void DrawRollbackPopup();
};

} // namespace cccaster::domain::ui
