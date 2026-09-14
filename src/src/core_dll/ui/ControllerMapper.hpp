// ============================================================================
// ControllerMapper.hpp — コントローラーマッピングUI（業務ロジック）
// ============================================================================
//
// 【設計思想】
//   F4キーで開閉するコントローラー設定画面の描画＋入力処理を担う。
//   OverlayRenderer（描画基盤）を利用し、NetplayOverlay（表示層）から
//   呼び出される片方向依存設計。
//
// 【ウィンドウ構成】
//   ┌─── メインウィンドウ (620px幅) ─────────────┐
//   │ CONTROLLER CONFIGURATION                      │
//   │ P1 CONTROLLER │ AVAILABLE DEVICES │ P2 CTRL   │
//   └────────────────────────────────────────────────┘
//   ┌── P1 Binding ──┐           ┌── P2 Binding ──┐
//   │ > Up : W        │           │      W : Up <   │
//   └─────────────────┘           └─────────────────┘
//
// 【依存関係】
//   - OverlayRenderer  : PushModernStyle/PopModernStyle/DrawFittedText
//   - DirectInputHook  : デバイス列挙・エッジ検出
//   - ConfigManager    : INI保存
//   - ImGui
// ============================================================================
#pragma once
#include <string>
#include <imgui.h>

namespace cccaster::domain::ui {

/// @brief コントローラーマッピング画面の描画・入力処理を行う。
/// @details
///   全メソッド・状態が static。NetplayOverlay::Render() から
///   showMappingWindow == true の時に Draw() が呼ばれる。
class ControllerMapper {
  public:
    /// マッピングで定義するゲーム入力の総数 (Up〜A+B の13種)
    static constexpr int NUM_GAME_INPUTS = 13;

    /// マッピングウィンドウのベース幅 [px]
    static constexpr float MAPPING_WINDOW_WIDTH = 620.0f;

    /// マッピング開始後の入力無視期間 [秒]
    static constexpr double MAPPING_START_DEAD_TIME = 0.2;

    /// @brief マッピングウィンドウ全体を描画・制御する。
    static void Draw();

    /// @brief 現在のP1/P2デバイス割当をINIに保存し、DirectInputHookを再読込。
    static void SaveDeviceAllocations();

    /// @brief バインド途中の状態を安全にリセットする。
    /// @details F4 でウィンドウを閉じた際に OnMappingInput() から呼ばれる。
    ///          P1/P2 のバインドステップ位置とバインド文字列をクリアする。
    static void ResetBindingState();

  private:
    // ------------------------------------------------------------------
    // 入力処理ヘルパー
    // ------------------------------------------------------------------

    /// @brief デバイス選択フェーズのジョイスティック/キーボード入力を処理し、
    ///        g_p1JoyId / g_p2JoyId を更新する。
    static void ProcessDeviceSelectionInput();

    /// @brief バインド設定フェーズの入力を処理する。
    /// @param joyId   対象プレイヤーのジョイスティックID (-2=kbd, 0+=gamepad)
    /// @param playerIndex  0=P1, 1=P2
    /// @param pos     バインドステップ位置（参照: 0=未開始, 1-13=入力中, 14=完了確認）
    /// @param binds   バインド文字列配列（NUM_GAME_INPUTS 要素）
    static void ProcessBindingInput(int joyId, int playerIndex, int &pos, std::string *binds);

    /// @brief バインド完了時にデバイスINIとcccaster.iniへ保存する。
    /// @param joyId   対象プレイヤーのジョイスティックID
    /// @param prefix  "P1" or "P2"
    /// @param binds   保存するバインド文字列配列
    static void SaveBinds(int joyId, const std::string &prefix, std::string *binds);

    // ------------------------------------------------------------------
    // 描画ヘルパー
    // ------------------------------------------------------------------

    /// @brief デバイス割当テーブル (3カラム: P1 / 利用可能デバイス / P2) を描画する。
    static void DrawDeviceSelectionTable();

    /// @brief P1バインドウィンドウを描画する。表示中のみ呼ばれる。
    static void DrawP1BindingWindow(float screenWidth);

    /// @brief P2バインドウィンドウを描画する。表示中のみ呼ばれる。
    static void DrawP2BindingWindow(float screenWidth);

    /// @brief 1人分のバインドリスト (Up〜Finish) を描画するユーティリティ。
    /// @param pos     現在のバインドステップ位置
    /// @param binds   バインド文字列配列
    /// @param hiliteColor ハイライト色（P1/P2で異なる）
    /// @param rightAlign  右揃え描画か（P2=true）
    static void DrawBindList(int pos, std::string *binds, ImVec4 hiliteColor, bool rightAlign);
};

} // namespace cccaster::domain::ui
