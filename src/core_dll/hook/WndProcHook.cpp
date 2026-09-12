#include "core_dll/hook/WndProcHook.hpp"
#include "core_dll/ui/UIManager.hpp"
#include "shared_contracts/IpcData.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/sync/InputTimeline.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include <cstdlib>
#include <cstdio>
#include <windowsx.h>

void HookLog(const char *msg);

using namespace cccaster::game_interface;

WNDPROC WndProcHook::original_WndProc = nullptr;
HWND WndProcHook::hooked_hwnd = nullptr;

namespace {
// ゲームのウィンドウスレッドだけが所有する。標準の移動用モーダルループへ入らない。
struct CaptionDrag {
    bool active = false;
    bool escapeHeld = false;
    POINT cursor{}, origin{};
    unsigned moves = 0;
} drag;

void TraceDrag(const char *event) {
    static const bool trace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
    if (!trace) return;
    char message[192];
    snprintf(message, sizeof(message), "[WindowDrag] event=%s ticks=%lld applied=%u sampled=%u moves=%u",
        event, cccaster::platform::RealMonotonicTicks(),
        cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load(),
        cccaster::core::sync::InputTimeline::GetInstance().SampledFrame(), drag.moves);
    HookLog(message);
}
void TraceMenu(const char *event) {
    static const bool trace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
    if (!trace) return;
    char message[96];
    snprintf(message, sizeof(message), "[WindowMenu] event=%s", event);
    HookLog(message);
}
void FinishDrag(HWND hwnd, const char *reason, bool restore = false) {
    if (!drag.active) return;
    drag.active = false; // ReleaseCapture/SetWindowPosからの再入より前に終了する。
    if (restore)
        SetWindowPos(hwnd, nullptr, drag.origin.x, drag.origin.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (GetCapture() == hwnd) ReleaseCapture();
    TraceDrag(reason);
}
bool BeginDrag(HWND hwnd, LPARAM position) {
    // 最大化中の復元移動はWindowsへ任せる。対象ゲームの通常窓は固定サイズ。
    if (drag.active || IsZoomed(hwnd) || GetCapture()) return false;
    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) return false;
    drag.cursor = {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    drag.origin = {rect.left, rect.top};
    drag.moves = 0;
    SetCapture(hwnd);
    if (GetCapture() != hwnd) return false;
    drag.active = true;
    TraceDrag("begin");
    return true;
}
void MoveDrag(HWND hwnd, LPARAM position) {
    // 捕捉中のWM_MOUSEMOVE/WM_LBUTTONUPはクライアント座標。負の画面座標も保持する。
    POINT cursor{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (!ClientToScreen(hwnd, &cursor)) return;
    if (SetWindowPos(hwnd, nullptr, drag.origin.x + cursor.x - drag.cursor.x,
                     drag.origin.y + cursor.y - drag.cursor.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) ++drag.moves;
}
}

bool WndProcHook::Initialize(HWND hwnd) {
    if (original_WndProc)
        return true;

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (!hwnd || windowPid != GetCurrentProcessId()) return false;

    hooked_hwnd = hwnd;
    original_WndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWindowProc);

    char buf[128];
    if (!original_WndProc) {
        snprintf(buf, sizeof(buf), "[InputHook] SetWindowLongPtr FAILED for HWND %p, error: %lu", hwnd,
                 GetLastError());
    } else {
        snprintf(buf, sizeof(buf), "[InputHook] SetWindowLongPtr SUCCEEDED for HWND %p", hwnd);
    }
    HookLog(buf);

    return original_WndProc != nullptr;
}

bool WndProcHook::BlocksEscapeExit() {
    return drag.active || drag.escapeHeld;
}

void WndProcHook::PumpMessages() {
    static bool pumping = false;
    if (!hooked_hwnd || GetWindowThreadProcessId(hooked_hwnd, nullptr) != GetCurrentThreadId() || pumping)
        return;
    pumping = true;
    // フィルター付きPM_NOREMOVEによる終了確認だけでは、通常メッセージが残り得る。
    // Present完了後・次のゲーム更新前に処理し、再入と大量投稿による無制限ループを防ぐ。
    MSG message{};
    for (unsigned count = 0; count < 64 &&
         PeekMessage(&message, hooked_hwnd, 0, 0, PM_REMOVE); ++count) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam)); // 元のゲームループへ残す。
            break;
        }
        TranslateMessage(&message);
        DispatchMessage(&message);
        if (!hooked_hwnd) break;
    }
    pumping = false;
}

void WndProcHook::Shutdown() {
    if (original_WndProc && hooked_hwnd) {
        FinishDrag(hooked_hwnd, "shutdown");
        drag.escapeHeld = false;
        SetWindowLongPtr(hooked_hwnd, GWLP_WNDPROC, (LONG_PTR)original_WndProc);
        original_WndProc = nullptr;
        hooked_hwnd = nullptr;
    }
}

LRESULT CALLBACK WndProcHook::HookedWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    using namespace cccaster::public_api;
    // 標準メニューもゲームスレッドを占有する。ユーザー承認により入口を無効化する。
    // アイコン押下自体の追跡も避け、右上の閉じるボタン/SC_CLOSEは後段で従来どおり扱う。
    if (((uMsg == WM_NCLBUTTONDOWN || uMsg == WM_NCLBUTTONUP || uMsg == WM_NCLBUTTONDBLCLK) &&
         wParam == HTSYSMENU) ||
        ((uMsg == WM_NCRBUTTONDOWN || uMsg == WM_NCRBUTTONUP) &&
         (wParam == HTCAPTION || wParam == HTSYSMENU)) ||
        (uMsg == WM_SYSCOMMAND &&
         ((wParam & 0xfff0) == SC_MOUSEMENU || (wParam & 0xfff0) == SC_KEYMENU)) ||
        (uMsg == WM_CONTEXTMENU && reinterpret_cast<HWND>(wParam) == hWnd)) {
        TraceMenu("blocked");
        return 0;
    }
    if (uMsg == WM_ENTERMENULOOP || uMsg == WM_EXITMENULOOP)
        TraceMenu(uMsg == WM_ENTERMENULOOP ? "begin" : "end");
    // 別窓へ移った後のキーアップを受け取れなくても、終了抑止を残さない。
    if (uMsg == WM_KILLFOCUS) drag.escapeHeld = false;
    // Esc取消のリピート/キーアップをゲーム終了やF4設定へ漏らさない。
    if (drag.escapeHeld && wParam == VK_ESCAPE &&
        (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP)) {
        if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) drag.escapeHeld = false;
        return 0;
    }
    if (uMsg == WM_NCLBUTTONDOWN && wParam == HTCAPTION && BeginDrag(hWnd, lParam))
        return 0;
    if (drag.active) {
        if (uMsg == WM_MOUSEMOVE) {
            if (wParam & MK_LBUTTON) MoveDrag(hWnd, lParam);
            else FinishDrag(hWnd, "button-lost");
            return 0;
        }
        if (uMsg == WM_LBUTTONUP) {
            MoveDrag(hWnd, lParam);
            FinishDrag(hWnd, "end");
            return 0;
        }
        if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
            drag.escapeHeld = true;
            FinishDrag(hWnd, "cancel", true);
            return 0;
        }
        if (uMsg == WM_CANCELMODE) FinishDrag(hWnd, "cancel-mode", true);
        if (uMsg == WM_CAPTURECHANGED) FinishDrag(hWnd, "capture-lost");
        if (uMsg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) FinishDrag(hWnd, "deactivate");
        if (uMsg == WM_CLOSE || uMsg == WM_NCDESTROY) FinishDrag(hWnd, "close");
    }
    static const bool moveTrace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
    if (moveTrace && (uMsg == WM_ENTERSIZEMOVE || uMsg == WM_EXITSIZEMOVE)) {
        char message[192];
        snprintf(message, sizeof(message), "[WindowMove] event=%s ticks=%lld applied=%u sampled=%u",
            uMsg == WM_ENTERSIZEMOVE ? "begin" : "end", cccaster::platform::RealMonotonicTicks(),
            cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load(),
            cccaster::core::sync::InputTimeline::GetInstance().SampledFrame());
        HookLog(message);
    }
    if ((uMsg == WM_CLOSE || (uMsg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE)) &&
        RequestLocalGameExit(SessionExitReason::CloseButton)) return 0;
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE &&
        !cccaster::domain::ui::UIManager::IsMappingWindowOpen() &&
        RequestLocalGameExit(SessionExitReason::Escape)) return 0;
    // UI 側に処理を委譲
    int result = cccaster::domain::ui::UIManager::HandleWndProcMessage(hWnd, uMsg, wParam, lParam);
    if (result > 0)
        return 0; // ブロック
    if (result == 0) {
    } // ゲームに通す（フォールスルー）

    // 元の WndProc に委譲
    return CallWindowProc(original_WndProc, hWnd, uMsg, wParam, lParam);
}
