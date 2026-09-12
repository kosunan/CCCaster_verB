#include "core_dll/sync/SettingsCommands.hpp"
// ============================================================================
// UIManager.cpp — 画面切替エントリポイント実装
// ============================================================================

#include "core_dll/ui/UIManager.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/CharaSelect_Ui_View.hpp"
#include "core_dll/ui/InGame_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include <imgui.h>
#include <dbt.h> // mingw-w64 のヘッダ名は小文字。case-sensitive FS でのクロスビルド用

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cccaster::domain::ui {

void UIManager::Render(UiPhase phase) {
    // 相手側の画面遷移などで設定画面を離れても、入力遮断状態を次画面へ持ち越さない。
    // 強制遷移では自動保存しない。未保存ドラフトだけ次回F4へ保持する。
    const bool training = cccaster::domain::session::SceneRunner::AppMode() == 1;
    if (phase != UiPhase::CharaSelect && !(training && phase == UiPhase::InGame) && StateUiLogic::IsMappingWindowOpen()) {
        StateUiLogic::CloseMappingWindow();
        ControllerUiLogic::Suspend();
    }
    if (StateUiLogic::IsMappingWindowOpen()) { ControllerUiView::Draw(); return; }
    switch (phase) {
    case UiPhase::CharaSelect:
        CharaSelectUiView::Draw();
        break;
    case UiPhase::InGame:
        InGameUiView::Draw();
        break;
    case UiPhase::Rematch:
        // 再戦はゲーム本来のメニューを使用する。
        StateUiView::DrawRematchBar();
        break;
    case UiPhase::None:
    default:
        break;
    }
}

// --- 入力イベント委譲 ---
void UIManager::OnDelayInput(int num) {
    auto &mem = cccaster::game_interface::GameMem();
    if (mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
        cccaster::core::netplay::NetplaySession::GetInstance().IsRunning())
        cccaster::core::sync::SettingsCommands::Request(false, num);
}
void UIManager::OnRollbackInput(int num) {
    auto &mem = cccaster::game_interface::GameMem();
    if (mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
        cccaster::core::netplay::NetplaySession::GetInstance().IsRunning())
        cccaster::core::sync::SettingsCommands::Request(true, num);
}

void UIManager::OnMappingInput() {
    if (StateUiLogic::IsMappingWindowOpen()) {
        if (ControllerUiView::OnClose()) StateUiLogic::CloseMappingWindow();
    } else {
        StateUiLogic::ToggleMappingWindow();
    }
}

bool UIManager::IsMappingWindowOpen() {
    return StateUiLogic::IsMappingWindowOpen();
}

// ============================================================================
// HandleWndProcMessage — WndProc メッセージの UI 側処理
//   WndProcHook から呼ばれる。フック基盤（SetWindowLongPtr）は hook/ に残し、
//   UI 固有ロジック（ImGui, ホットキー, マッピング, デバイス検出）をここに集約。
//
//   @return >0: ゲームにブロック, 0: ゲームに通す, -1: 判定なし(元 WndProc に委譲)
// ============================================================================

int UIManager::HandleWndProcMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // F1は表示だけを切替。長押しと解放をゲームへ通さない。
    static bool frameBarF1Held = false;
    if (uMsg == WM_KILLFOCUS) frameBarF1Held = false;
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && wParam == VK_F1 && frameBarF1Held) {
        frameBarF1Held = false;
        return 1;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_F1 &&
        FrameBarDisplay::Available(cccaster::domain::session::SceneRunner::AppMode())) {
        frameBarF1Held = true;
        if (!(lParam & (1u << 30)) && !IsMappingWindowOpen() &&
            !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
            FrameBarDisplay::Toggle();
        return 1;
    }
    static bool hudF3Held = false;
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && wParam == VK_F3 && hudF3Held) {
        hudF3Held = false;
        return 1;
    }
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_F3 && hudF3Held)
        return 1;
    // (1) ImGui に渡す
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
        return 1; // ImGui が消費
    }

    // (2) ホットキー処理
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        // リピート除外: lParam bit30=1 → 前回もキーダウン
        bool isRepeat = (lParam & (1 << 30)) != 0;
        if (isRepeat) {
            if (IsMappingWindowOpen())
                return 1; // マッピング中はブロック
            return -1;    // リピートはゲームに通す
        }

        bool isCtrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool isAltDown = (uMsg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
        int key = static_cast<int>(wParam);
        if (isCtrlDown && isAltDown)
            return -1; // AltGrを設定操作として扱わない。

        // F4: キャラセレ、またはオフライントレーニングの設定。
        if (key == VK_F4 && !isAltDown) {
            auto &mem = cccaster::game_interface::GameMem();
            const bool trainingBattle = cccaster::domain::session::SceneRunner::AppMode() == 1 &&
                                        mem.GameMode() == CC_GAME_MODE_IN_GAME;
            if (!mem.IsAvailable() || (mem.GameMode() != CC_GAME_MODE_CHARA_SELECT && !trainingBattle)) {
                return 1; // キャラセレ以外では無視
            }
            OnMappingInput();
            return 1;
        }

        // マッピング中は全キーブロック
        if (IsMappingWindowOpen())
            return 1;

        // Ctrl+F3: 表示だけを変更。ゲーム入力や同期設定には触れない。
        if (key == VK_F3 && isCtrlDown && !isAltDown) {
            hudF3Held = true;
            StateUiView::CycleDisplayMode();
            return 1;
        }

        // Ctrl+数字: Delay / Alt+数字: Rollback
        if (key >= '0' && key <= '9') {
            int num = key - '0';
            if (isCtrlDown) {
                OnDelayInput(num);
                return 1;
            }
            if (isAltDown) {
                OnRollbackInput(num);
                return 1;
            }
        }
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
            int num = key - VK_NUMPAD0;
            if (isCtrlDown) {
                OnDelayInput(num);
                return 1;
            }
            if (isAltDown) {
                OnRollbackInput(num);
                return 1;
            }
        }
    }

    // (3) マッピング中は KEYUP もブロック
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && IsMappingWindowOpen()) {
        return 1;
    }

    // (4) USB デバイス挿抜 → コントローラ再検出
    if (uMsg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            cccaster::game_interface::DirectInputHook::RequestDeviceRefresh();
        }
    }

    // (5) ImGui WantCapture
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse && (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST))
        return 1;
    if (io.WantCaptureKeyboard && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN ||
                                   uMsg == WM_SYSKEYUP || uMsg == WM_CHAR))
        return 1;

    return -1; // 判定なし → 元 WndProc に委譲
}

} // namespace cccaster::domain::ui
