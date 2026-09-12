#pragma once
// ============================================================================
// Controller_Ui_View — マッピング画面 描画
// ============================================================================
//
// 【責務】
//   F4で開閉するコントローラー設定画面の描画。
//   既存 ControllerMapper の Draw 系メソッドをラップ。
// ============================================================================

namespace cccaster::domain::ui {

class ControllerUiView {
  public:
    /// マッピング画面全体を描画
    static void Draw();

    /// 画面が閉じられた時のクリーンアップ
    static bool OnClose();
};

} // namespace cccaster::domain::ui
