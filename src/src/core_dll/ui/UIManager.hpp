#pragma once
// ============================================================================
// UIManager — 画面切替エントリポイント
// ============================================================================
//
// 【責務】
//   GamePhase に応じて適切な View を呼び出す。
//   DxHook::Hooked_EndScene() から Render() が毎フレーム呼ばれる。
// ============================================================================

#include <cstdint>
#include <windows.h>

namespace cccaster::domain::ui {

/// ゲーム画面フェーズ
enum class UiPhase : uint8_t {
    None = 0,    ///< UI非表示
    CharaSelect, ///< キャラセレ画面
    InGame,      ///< 対戦画面
    Rematch,     ///< 再戦画面
};

class UIManager {
  public:
    /// @brief 毎フレーム呼ばれる描画エントリポイント
    static void Render(UiPhase phase);

    /// @brief WndProc メッセージの UI 側処理
    /// @return >0: ゲームに渡さない(ブロック), 0: ゲームに渡す, -1: 判定なし(元 WndProc に委譲)
    static int HandleWndProcMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // --- 入力イベント（InputHook から呼ばれる） ---
    static void OnDelayInput(int num);
    static void OnRollbackInput(int num);
    static void OnMappingInput();

    /// @brief マッピングウィンドウが開いているか（InputHook からキーブロック判定に使用）
    static bool IsMappingWindowOpen();

    // --- データ更新（各モジュールから呼ばれる） ---
};

} // namespace cccaster::domain::ui
