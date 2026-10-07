#include "core_dll/hook/BorderlessDisplay.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

void HookLog(const char* message);

namespace cccaster::game_interface::borderless {
namespace {
HWND window = nullptr;
bool active = false, changing = false, enterHeld = false, startBorderless = false;
bool aspectQueryAvailable = true;
bool smoothScaling = true;
RECT savedClient{0, 0, 640, 480};
Resolution returnClient{640, 480};
LONG_PTR savedStyle = 0, savedExStyle = 0;
HMENU savedMenu = nullptr;
RECT savedRect{};
WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
IDirect3DSwapChain9* chain = nullptr;
UINT chainWidth = 0, chainHeight = 0;
UINT initialWidth = 640, initialHeight = 480;

void Trace(const char* event, const RECT& rect) {
    MONITORINFOEXA monitor{}; monitor.cbSize = sizeof(monitor);
    DEVMODEA mode{}; mode.dmSize = sizeof(mode);
    GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    char text[256];
    std::snprintf(text, sizeof(text), "[Borderless] %s rect=%ld,%ld,%ld,%ld monitor=%s mode=%lux%lu@%lu", event,
                  rect.left, rect.top, rect.right, rect.bottom, monitor.szDevice,
                  mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
    HookLog(text);
}
bool SetStyle(int index, LONG_PTR value) {
    SetLastError(0);
    return SetWindowLongPtr(window, index, value) != 0 || GetLastError() == 0;
}
bool MonitorBounds(RECT& rect) {
    MONITORINFO info{sizeof(info)};
    // 跨いでいれば最大の重なり、画面外なら一番近いモニター。
    if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return false;
    rect = info.rcMonitor; // 作業領域ではなくタスクバーを含めたモニター全域。
    return true;
}
void Restore() {
    changing = true;
    active = false;
    SetStyle(GWL_STYLE, savedStyle);
    SetStyle(GWL_EXSTYLE, savedExStyle);
    SetMenu(window, savedMenu);
    auto placement = savedPlacement;
    if (!(savedStyle & WS_VISIBLE)) placement.showCmd = SW_HIDE;
    SetWindowPlacement(window, &placement);
    // 通常窓では外接矩形を正確に戻す。最大化状態はWINDOWPLACEMENTで復元。
    const UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE |
        (savedPlacement.showCmd == SW_SHOWMAXIMIZED ? SWP_NOMOVE | SWP_NOSIZE : 0);
    SetWindowPos(window, savedExStyle & WS_EX_TOPMOST ? HWND_TOPMOST : HWND_NOTOPMOST,
                 savedRect.left, savedRect.top, savedRect.right - savedRect.left,
                 savedRect.bottom - savedRect.top, flags);
    changing = false;
    ReleaseResources();
    Trace("windowed", savedRect);
}
bool Enter() {
    RECT bounds{};
    if (!MonitorBounds(bounds) || !GetWindowRect(window, &savedRect) ||
        !GetWindowPlacement(window, &savedPlacement) || !GetClientRect(window, &savedClient) ||
        savedClient.right <= 0 || savedClient.bottom <= 0) return false;
    savedStyle = GetWindowLongPtr(window, GWL_STYLE);
    savedExStyle = GetWindowLongPtr(window, GWL_EXSTYLE);
    savedMenu = GetMenu(window);
    returnClient = {int(savedClient.right), int(savedClient.bottom)};
    changing = true;
    active = true;
    const bool ok = SetStyle(GWL_STYLE, (savedStyle & ~(WS_OVERLAPPEDWINDOW | WS_MINIMIZE | WS_MAXIMIZE)) | WS_POPUP) &&
        SetStyle(GWL_EXSTYLE, savedExStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME)) &&
        SetMenu(window, nullptr) &&
        SetWindowPos(window, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    changing = false;
    if (!ok) { Restore(); return false; }
    Trace("enabled", bounds);
    return true;
}
void Refit() {
    RECT bounds{};
    if (!active || changing || IsIconic(window) || !MonitorBounds(bounds)) return;
    changing = true;
    SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                 bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    changing = false;
    ReleaseResources();
}
}

RECT FitAspect(LONG width, LONG height, LONG sourceWidth, LONG sourceHeight) {
    if (width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return {};
    LONG w = width, h = height;
    if (int64_t(width) * sourceHeight > int64_t(height) * sourceWidth)
        w = LONG(int64_t(height) * sourceWidth / sourceHeight);
    else h = LONG(int64_t(width) * sourceHeight / sourceWidth);
    const LONG x = (width - w) / 2, y = (height - h) / 2;
    return {x, y, x + w, y + h};
}
RECT FitContent(LONG width, LONG height) { return FitAspect(width, height, savedClient.right, savedClient.bottom); }
bool Active() { return active; }
void SetScaleFilter(bool enabled) { smoothScaling = enabled; }
DisplaySettings GetDisplaySettings() {
    if (!aspectQueryAvailable || !IsWindow(window) || changing) return {};
    RECT client{};
    if (!GetClientRect(window, &client)) return {};
    return {true, active, active ? returnClient : Resolution{int(client.right), int(client.bottom)}};
}
bool SetFullscreen(bool enabled) {
    if (!GetDisplaySettings().available) return false;
    if (active == enabled) return true;
    if (enabled) return Enter();
    Restore();
    return !active;
}
Resolution ResolutionLimit() {
    const auto state = GetDisplaySettings();
    if (!state.available) return {};
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return {};
    const auto style = (active ? savedStyle : GetWindowLongPtr(window, GWL_STYLE)) & ~(WS_MAXIMIZE | WS_MINIMIZE);
    const auto exStyle = active ? savedExStyle : GetWindowLongPtr(window, GWL_EXSTYLE);
    const auto menu = active ? savedMenu : GetMenu(window);
    RECT frame{};
    if (!AdjustWindowRectEx(&frame, DWORD(style), menu != nullptr, DWORD(exStyle))) return {};
    const auto &work = info.rcWork;
    return {int(work.right-work.left-(frame.right-frame.left)),
            int(work.bottom-work.top-(frame.bottom-frame.top))};
}
bool ApplyRenderResolution(Resolution size) {
    const auto state = GetDisplaySettings();
    if (!state.available || size.width <= 0 || size.height <= 0) return false;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return false;
    const auto style = (active ? savedStyle : GetWindowLongPtr(window, GWL_STYLE)) & ~(WS_MAXIMIZE | WS_MINIMIZE);
    const auto exStyle = active ? savedExStyle : GetWindowLongPtr(window, GWL_EXSTYLE);
    const auto menu = active ? savedMenu : GetMenu(window);
    RECT frame{};
    if (!AdjustWindowRectEx(&frame, DWORD(style), menu != nullptr, DWORD(exStyle))) return false;
    const auto &work = info.rcWork;
    RECT rect = active ? savedRect : RECT{};
    if (!active && !GetWindowRect(window, &rect)) return false;
    const LONG w = size.width + frame.right-frame.left;
    const LONG h = size.height + frame.bottom-frame.top;
    const LONG x = std::clamp(rect.left+(rect.right-rect.left-w)/2, work.left, std::max(work.left,work.right-w));
    const LONG y = std::clamp(rect.top+(rect.bottom-rect.top-h)/2, work.top, std::max(work.top,work.bottom-h));
    if (active) {
        // 全画面の描画元も新しいバックバッファの比率へ即時更新する。
        savedClient = {0,0,size.width,size.height};
        savedRect = {x,y,x+w,y+h};
        savedStyle = style;
        savedPlacement.showCmd = SW_SHOWNORMAL;
        returnClient = size;
        ReleaseResources();
        Trace("render-resolution", savedClient);
        return true;
    }
    changing = true;
    if (IsZoomed(window) || IsIconic(window)) ShowWindow(window, SW_RESTORE);
    const bool ok = SetWindowPos(window, nullptr, x,y,w,h,SWP_NOZORDER | SWP_NOACTIVATE);
    changing = false;
    RECT client{};
    const bool applied = ok && GetClientRect(window, &client) && client.right == size.width && client.bottom == size.height;
    if (applied) Trace("resolution", client);
    return applied;
}
void SetAspectQueryAvailable(bool available) { aspectQueryAvailable = available; }
bool RenderingClientRect(HWND hwnd, RECT& rect) {
    if (!active || hwnd != window) return false;
    rect = savedClient;
    return true;
}
void ConfigureDevice(HWND hwnd, D3DPRESENT_PARAMETERS& parameters, bool creating) {
    if (!aspectQueryAvailable) return;
    if (creating) {
        startBorderless = !parameters.Windowed;
        initialWidth = parameters.BackBufferWidth ? parameters.BackBufferWidth : 640;
        initialHeight = parameters.BackBufferHeight ? parameters.BackBufferHeight : 480;
    }
    // 保存済みWindowed=0からの起動とデバイス復旧でも排他モードに入れない。
    parameters.Windowed = TRUE;
    parameters.FullScreen_RefreshRateInHz = 0;
    if (!parameters.hDeviceWindow) parameters.hDeviceWindow = hwnd;
    if (parameters.SwapEffect == D3DSWAPEFFECT_FLIP) parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    // 同一リプレイで表示経路を比較する検証専用。通常起動の設定は保持する。
    const char* interval = std::getenv("CCCASTER_TEST_PRESENT_INTERVAL");
    if (interval && !std::strcmp(interval, "one"))
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
}
void Attach(HWND hwnd) {
    if (!aspectQueryAvailable) return;
    if (window == hwnd) return;
    window = hwnd;
    if (!startBorderless) return;
    startBorderless = false;
    // 元ゲームが全画面用の枠なし窓を作った場合、戻り先の通常窓を用意する。
    RECT bounds{};
    if (!(GetWindowLongPtr(window, GWL_STYLE) & WS_CAPTION) && MonitorBounds(bounds)) {
        const auto style = (GetWindowLongPtr(window, GWL_STYLE) & ~(WS_POPUP | WS_MAXIMIZE)) |
                           WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT normal{0, 0, LONG(initialWidth), LONG(initialHeight)};
        AdjustWindowRectEx(&normal, DWORD(style), GetMenu(window) != nullptr,
                           DWORD(GetWindowLongPtr(window, GWL_EXSTYLE)));
        SetStyle(GWL_STYLE, style);
        const auto w = normal.right - normal.left, h = normal.bottom - normal.top;
        SetWindowPos(window, nullptr, bounds.left + (bounds.right - bounds.left - w) / 2,
                     bounds.top + (bounds.bottom - bounds.top - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    Enter();
}
void Detach() {
    if (active && IsWindow(window)) Restore();
    ReleaseResources();
    window = nullptr;
    active = changing = enterHeld = startBorderless = false;
}
bool HandleMessage(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (hwnd != window) return false;
    if ((active && message == WM_ACTIVATEAPP) || message == WM_DISPLAYCHANGE) {
        RECT rect{}; GetWindowRect(window, &rect);
        Trace(message == WM_DISPLAYCHANGE ? "display-change" : w ? "foreground" : "background", rect);
    }
    if (message == WM_KILLFOCUS) enterHeld = false;
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && w == VK_RETURN && enterHeld) {
        enterHeld = false;
        return true;
    }
    if ((message == WM_SYSCHAR || message == WM_SYSKEYUP) && w == VK_RETURN && (l & (1u << 29)))
        return true;
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && w == VK_RETURN &&
        (enterHeld || (l & (1u << 29)) || (GetKeyState(VK_MENU) & 0x8000))) {
        enterHeld = true;
        if (!(l & (1u << 30)) && !changing) {
            SetFullscreen(!active);
        }
        return true;
    }
    if (active && !changing && (message == WM_DISPLAYCHANGE || message == WM_DPICHANGED)) {
        Refit();
        return true;
    }
    return false;
}
void ReleaseResources() {
    if (chain) chain->Release();
    chain = nullptr;
    chainWidth = chainHeight = 0;
}
bool Present(IDirect3DDevice9* device, HRESULT& result, DWORD flags) {
    if (!active) return false;
    RECT client{};
    if (IsIconic(window) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        result = D3D_OK;
        return true;
    }
    result = D3D_OK;
    if (!chain || chainWidth != UINT(client.right) || chainHeight != UINT(client.bottom)) {
        ReleaseResources();
        IDirect3DSwapChain9* original = nullptr;
        D3DPRESENT_PARAMETERS parameters{};
        result = device->GetSwapChain(0, &original);
        if (SUCCEEDED(result)) { result = original->GetPresentParameters(&parameters); original->Release(); }
        if (SUCCEEDED(result)) {
            parameters.BackBufferWidth = client.right;
            parameters.BackBufferHeight = client.bottom;
            parameters.BackBufferCount = 1;
            parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
            parameters.MultiSampleQuality = 0;
            parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
            parameters.Windowed = TRUE;
            parameters.hDeviceWindow = window;
            parameters.EnableAutoDepthStencil = FALSE;
            parameters.FullScreen_RefreshRateInHz = 0;
            parameters.Flags = 0;
            result = device->CreateAdditionalSwapChain(&parameters, &chain);
        }
        if (SUCCEEDED(result)) {
            chainWidth = client.right; chainHeight = client.bottom;
            Trace("surface", client);
        }
    }
    IDirect3DSurface9* source = nullptr;
    IDirect3DSurface9* destination = nullptr;
    if (SUCCEEDED(result)) result = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &source);
    if (SUCCEEDED(result)) result = chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &destination);
    if (SUCCEEDED(result)) result = device->ColorFill(destination, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    const RECT content = FitContent(client.right, client.bottom);
    if (SUCCEEDED(result)) {
        result = device->StretchRect(source, nullptr, destination, &content, smoothScaling ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        if (result == D3DERR_INVALIDCALL) // 線形拡大非対応のドライバ向け。
            result = device->StretchRect(source, nullptr, destination, &content, D3DTEXF_NONE);
    }
    if (destination) destination->Release();
    if (source) source->Release();
    if (SUCCEEDED(result)) result = chain->Present(nullptr, nullptr, window, nullptr, flags);
    if (FAILED(result) && result != D3DERR_DEVICELOST && result != D3DERR_DEVICENOTRESET &&
        result != D3DERR_WASSTILLDRAWING) {
        char text[128];
        std::snprintf(text, sizeof(text), "[Borderless] presentation failed hr=0x%08lX; restoring window", (unsigned long)result);
        HookLog(text);
        Restore();
        return false;
    }
    return true;
}
}
