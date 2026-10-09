#include "core_dll/sync/SettingsCommands.hpp"
// ============================================================================
// UIManager.cpp — 画面切替エントリポイント実装
// ============================================================================

#include "core_dll/ui/UIManager.hpp"
#include "core_dll/ui/TrainingCharacterView.hpp"
#include "core_dll/ui/TrainingPaletteView.hpp"
#include "core_dll/ui/TrainingHitboxView.hpp"
#include "core_dll/mbaa_mem/TrainingHitboxMenu.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/ui/TrainingStandbyView.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/CharaSelect_Ui_View.hpp"
#include "core_dll/ui/InGame_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/engine/SelectionOptions.hpp"
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
    // 登録済みの項目は都度保存済み。強制遷移では入力待ちだけ中止する。
    const bool training = cccaster::domain::session::SceneRunner::AppMode() == 1;
    if (training && training_standby_view::Draw()) return;
    const bool replayList = cccaster::domain::session::SceneRunner::AppMode() == 4 &&
        cccaster::game_interface::GameMem().GameMode() == CC_GAME_MODE_REPLAY;
    if (phase != UiPhase::CharaSelect && !(training && phase == UiPhase::InGame) && !replayList && StateUiLogic::IsMappingWindowOpen()) {
        StateUiLogic::CloseMappingWindow();
        ControllerUiLogic::Suspend();
    }
    if (StateUiLogic::IsMappingWindowOpen()) { ControllerUiView::Draw(); return; }
    if (training && phase == UiPhase::InGame && training_palette_view::Draw()) return;
    if (training && phase == UiPhase::InGame && training_character_view::Draw()) return;
    if (training && phase == UiPhase::InGame && training_hitbox_view::Draw()) return;
    // 標準Trainingメニュー中は名前・勝数・詳細情報を重ねない。HUDの選択モードは保持する。
    if (training && phase == UiPhase::InGame && cccaster::game_interface::GameMem().IsPauseMenuOpen()) return;
    if (cccaster::domain::session::SceneRunner::AppMode() == 4 && phase != UiPhase::InGame) return;
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
    const auto appMode = cccaster::domain::session::SceneRunner::AppMode();
    if (mem.IsAvailable() && (appMode == 1 || appMode == 5) &&
        (mem.GameMode() == CC_GAME_MODE_CHARA_SELECT || (appMode == 1 && mem.GameMode() == CC_GAME_MODE_IN_GAME))) {
        cccaster::domain::session::SceneRunner::RequestLocalDelay(num);
        return;
    }
    if (mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
        cccaster::core::netplay::NetplaySession::GetInstance().IsRunning())
        cccaster::core::sync::SettingsCommands::Request(false, num);
}
void UIManager::OnRollbackInput(int num) {
    (void)num;
    auto &mem = cccaster::game_interface::GameMem();
    if (mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
        cccaster::core::netplay::NetplaySession::GetInstance().IsRunning())
        cccaster::core::sync::SettingsCommands::notice = 4; // 通常対戦は当面R7固定。
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
    namespace hitbox = cccaster::training_hitbox;
    if (uMsg == WM_KILLFOCUS) hitbox::escapeHeld = false;
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && wParam == VK_ESCAPE && hitbox::escapeHeld.exchange(false)) return 1;
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE && (hitbox::Active() || hitbox::escapeHeld)) {
        hitbox::escapeHeld = true;
        if (!(lParam & (1u << 30))) hitbox::escapeRequested = true;
        return 1;
    }
    if (uMsg == WM_KILLFOCUS) cccaster::training_palette::escapeHeld = false;
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && wParam == VK_ESCAPE && cccaster::training_palette::escapeHeld) {
        cccaster::training_palette::escapeHeld = false;
        ImGui_ImplWin32_WndProcHandler(hWnd,uMsg,wParam,lParam);
        return 1;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE && cccaster::training_palette::Active())
        cccaster::training_palette::escapeHeld = true;
    if (training_standby_view::Active() && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN ||
        uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP || uMsg == WM_CHAR)) {
        if (uMsg == WM_KEYDOWN) training_standby_view::Key(static_cast<unsigned>(wParam), (lParam & (1u << 30)) != 0);
        return 1;
    }
    namespace options = cccaster::domain::scene::selection_options;
    // キャラ選択は設定、Trainingはフレームバー、それ以外はHUD。長押し・解放は遮断する。
    static bool hudF1Held = false;
    if (uMsg == WM_KILLFOCUS) hudF1Held = false;
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && wParam == VK_F1 && hudF1Held) {
        hudF1Held = false;
        return 1;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_F1) {
        hudF1Held = true;
        if (!(lParam & (1u << 30)) && !IsMappingWindowOpen() &&
            !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            const auto mode = cccaster::domain::session::SceneRunner::AppMode();
            auto &mem = cccaster::game_interface::GameMem();
            if ((mode == 0 || mode == 1 || mode == 5) && mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT)
                options::Queue(options::Toggle);
            else if (mode == 1) FrameBarDisplay::ToggleTraining();
            else HudDisplay::Cycle();
        }
        return 1;
    }
    static bool hudF3Held = false;
    if (uMsg == WM_KILLFOCUS) {
        hudF3Held = false;
        options::heldKeys = 0;
        options::actions = 0;
    }
    if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        const auto mask = options::KeyMask(static_cast<unsigned>(wParam));
        if (options::heldKeys.fetch_and(~mask) & mask) return 1;
    }
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
    if (cccaster::training_palette::Active() && wParam != VK_F4 &&
        (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR)) return 1;

    // (2) ホットキー処理
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        // リピート除外: lParam bit30=1 → 前回もキーダウン
        bool isRepeat = (lParam & (1 << 30)) != 0;
        if (isRepeat) {
            if (IsMappingWindowOpen() || options::active.load())
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
            const bool replayList = cccaster::domain::session::SceneRunner::AppMode() == 4 &&
                                    mem.GameMode() == CC_GAME_MODE_REPLAY;
            if (!mem.IsAvailable() || (mem.GameMode() != CC_GAME_MODE_CHARA_SELECT && !trainingBattle && !replayList)) {
                return 1; // キャラセレ以外では無視
            }
            OnMappingInput();
            return 1;
        }

        // マッピング中は全キーブロック
        if (IsMappingWindowOpen())
            return 1;

        if (options::active.load() && !isAltDown) {
            options::heldKeys.fetch_or(options::KeyMask(key));
            if (key == VK_UP) options::Queue(options::Up);
            if (key == VK_DOWN) options::Queue(options::Down);
            if (key == VK_LEFT) options::Queue(options::Left);
            if (key == VK_RIGHT) options::Queue(options::Right);
            if (key == VK_ESCAPE || key == VK_RETURN) options::Queue(options::Close);
            return 1;
        }

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
    if ((uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) && (IsMappingWindowOpen() || options::active.load())) {
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
