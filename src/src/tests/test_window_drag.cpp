#include "tests/test_support.hpp"
#include "core_dll/hook/WndProcHook.hpp"
#include "core_dll/ui/UIManager.hpp"
#include "core_dll/sync/InputTimeline.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/common/Platform.hpp"
#include <windowsx.h>

// 非表示の自プロセス窓で本物のWndProcを検査。画面・実マウスは移動しない。
void HookLog(const char *) {}
namespace cccaster::platform {
int64_t RealMonotonicTicks() { return 1; }
}
namespace cccaster::domain::ui {
int UIManager::HandleWndProcMessage(HWND, UINT, WPARAM, LPARAM) { return 0; }
bool UIManager::IsMappingWindowOpen() { return false; }
}
namespace cccaster::core::sync {
InputTimeline &InputTimeline::GetInstance() { static InputTimeline s; return s; }
}
namespace cccaster::core::netplay {
NetplaySession &NetplaySession::GetInstance() { static NetplaySession s; return s; }
void NetplaySession::Stop() {}
}
namespace {
unsigned nativeCaption = 0, nativeModal = 0, updates = 0, escapeMessages = 0;
unsigned nativeMenu = 0, closeCommands = 0;
LRESULT CALLBACK Original(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP || message == WM_CONTEXTMENU ||
        (message == WM_SYSCOMMAND && ((w & 0xfff0) == SC_MOUSEMENU || (w & 0xfff0) == SC_KEYMENU))) {
        ++nativeMenu; return 0;
    }
    if (message == WM_SYSCOMMAND && (w & 0xfff0) == SC_CLOSE) { ++closeCommands; return 0; }
    if (message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONUP || message == WM_NCLBUTTONDBLCLK) {
        ++nativeCaption; return 0;
    }
    if (message == WM_ENTERSIZEMOVE) ++nativeModal;
    if (message == WM_APP+1) { ++updates; return 0; }
    if (message == WM_APP+2) { cccaster::game_interface::WndProcHook::PumpMessages(); return 0; }
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && w == VK_ESCAPE) ++escapeMessages;
    return DefWindowProc(hwnd, message, w, l);
}
RECT Position(HWND hwnd) { RECT r{}; GetWindowRect(hwnd,&r); return r; }
void Begin(HWND hwnd) {
    const auto r = Position(hwnd);
    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(r.left+100,r.top+10));
}
void MoveTo(HWND hwnd, int screenX, int screenY, UINT message = WM_MOUSEMOVE, WPARAM buttons = MK_LBUTTON) {
    POINT p{screenX,screenY}; ScreenToClient(hwnd,&p);
    SendMessage(hwnd,message,buttons,MAKELPARAM(p.x,p.y));
}
}
int main() {
    using cccaster::game_interface::WndProcHook;
    WNDCLASS wc{};
    wc.lpfnWndProc = Original;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "CCCasterWindowDragTest";
    CC_CHECK(RegisterClass(&wc));
    const auto window = CreateWindow(wc.lpszClassName, "", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                     -300,40,640,480,nullptr,nullptr,wc.hInstance,nullptr);
    const auto other = CreateWindow(wc.lpszClassName, "", WS_OVERLAPPED,0,0,10,10,
                                    nullptr,nullptr,wc.hInstance,nullptr);
    CC_CHECK(window && other);
    if (!window || !other) return cccaster::test::Summarize("window_drag");
    CC_CHECK(WndProcHook::Initialize(window));
    CC_CASE("通常フレーム境界のキュー処理は自窓だけを扱い再入と投稿過多を制限する");
    PostMessage(other,WM_APP+1,0,0);
    PostMessage(window,WM_APP+2,0,0);
    for (int i=0;i<100;++i) PostMessage(window,WM_APP+1,0,0);
    WndProcHook::PumpMessages();
    CC_CHECK(updates > 0 && updates <= 63); // 初回のOSメッセージも64件枠に含まれる。
    WndProcHook::PumpMessages();
    CC_CHECK_EQ(updates,100);
    MSG pending{};
    CC_CHECK(PeekMessage(&pending,other,WM_APP+1,WM_APP+1,PM_REMOVE));
    updates=0;
    while (PeekMessage(&pending,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&pending);
        DispatchMessage(&pending);
    }
    PostQuitMessage(17);
    WndProcHook::PumpMessages();
    CC_CHECK(PeekMessage(&pending,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE));
    CC_CHECK_EQ(pending.wParam,17);
    CC_CASE("標準メニューのマウス/キーボード入口を抑止し閉じる命令を維持する");
    SendMessage(window,WM_NCLBUTTONDOWN,HTSYSMENU,0);
    SendMessage(window,WM_NCLBUTTONUP,HTSYSMENU,0);
    SendMessage(window,WM_NCLBUTTONDBLCLK,HTSYSMENU,0);
    for (auto hit : {HTCAPTION, HTSYSMENU}) {
        SendMessage(window,WM_NCRBUTTONDOWN,hit,0);
        SendMessage(window,WM_NCRBUTTONUP,hit,0);
    }
    SendMessage(window,WM_SYSCOMMAND,SC_MOUSEMENU | 3,0);
    SendMessage(window,WM_SYSCOMMAND,SC_KEYMENU,' ');
    SendMessage(window,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(window),-1);
    CC_CHECK_EQ(nativeCaption,0);
    CC_CHECK_EQ(nativeMenu,0);
    SendMessage(window,WM_SYSCOMMAND,SC_CLOSE,0);
    CC_CHECK_EQ(closeCommands,1);
    CC_CASE("タイトルバー保持中も標準モーダル移動へ入らず通常処理へ戻る");
    const auto original = Position(window);
    Begin(window);
    CC_CHECK(GetCapture() == window);
    CC_CHECK(WndProcHook::BlocksEscapeExit());
    for (int i=0;i<180;++i) SendMessage(window,WM_APP+1,0,0);
    CC_CHECK_EQ(updates,180);
    CC_CHECK_EQ(nativeCaption,0);
    CC_CHECK_EQ(nativeModal,0);
    CC_CASE("負の画面座標と移動後のクライアント座標を正しく扱う");
    MoveTo(window,original.left+145,original.top-15);
    auto moved = Position(window);
    CC_CHECK_EQ(moved.left,original.left+45);
    CC_CHECK_EQ(moved.top,original.top-25);
    MoveTo(window,original.left+165,original.top+30,WM_LBUTTONUP,0);
    moved = Position(window);
    CC_CHECK_EQ(moved.left,original.left+65);
    CC_CHECK_EQ(moved.top,original.top+20);
    CC_CHECK(GetCapture() != window);
    CC_CHECK(!WndProcHook::BlocksEscapeExit());
    CC_CASE("Escで元位置に戻しリピートとキーアップをゲームへ漏らさない");
    const auto beforeCancel = Position(window);
    Begin(window);
    MoveTo(window,beforeCancel.left+180,beforeCancel.top+30);
    SendMessage(window,WM_KEYDOWN,VK_ESCAPE,0);
    CC_CHECK(WndProcHook::BlocksEscapeExit());
    SendMessage(window,WM_KEYDOWN,VK_ESCAPE,1LL<<30);
    SendMessage(window,WM_KEYUP,VK_ESCAPE,0);
    CC_CHECK(!WndProcHook::BlocksEscapeExit());
    CC_CHECK_EQ(escapeMessages,0);
    CC_CHECK_EQ(Position(window).left,beforeCancel.left);
    CC_CHECK_EQ(Position(window).top,beforeCancel.top);
    CC_CHECK(GetCapture() != window);
    CC_CASE("捕捉を別窓に渡した後は相手の捕捉を解除せず移動を終了する");
    Begin(window);
    SetCapture(other);
    CC_CHECK(GetCapture() == other);
    CC_CHECK(!WndProcHook::BlocksEscapeExit());
    MoveTo(window,0,0);
    CC_CHECK_EQ(Position(window).left,beforeCancel.left);
    ReleaseCapture();
    CC_CASE("ボタン解放通知の欠落とキャンセルからも捕捉を解放する");
    Begin(window);
    MoveTo(window,0,0,WM_MOUSEMOVE,0);
    CC_CHECK(GetCapture() != window);
    Begin(window);
    SendMessage(window,WM_CANCELMODE,0,0);
    CC_CHECK(GetCapture() != window);
    Begin(window);
    SendMessage(window,WM_ACTIVATE,WA_INACTIVE,0);
    CC_CHECK(GetCapture() != window);
    CC_CASE("解除時に捕捉とフックを残さずタイトルバー以外を横取りしない");
    Begin(window);
    SendMessage(window,WM_KEYDOWN,VK_ESCAPE,0);
    SendMessage(window,WM_KILLFOCUS,0,0);
    CC_CHECK(!WndProcHook::BlocksEscapeExit());
    SendMessage(window,WM_NCLBUTTONDOWN,HTCLOSE,0);
    CC_CHECK_EQ(nativeCaption,1);
    Begin(window);
    WndProcHook::Shutdown();
    CC_CHECK(GetCapture() != window);
    CC_CHECK(reinterpret_cast<WNDPROC>(GetWindowLongPtr(window,GWLP_WNDPROC)) == Original);
    DestroyWindow(window);
    DestroyWindow(other);
    UnregisterClass(wc.lpszClassName,wc.hInstance);
    return cccaster::test::Summarize("window_drag");
}
