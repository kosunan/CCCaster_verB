#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace cccaster::notification {
// 外部のbeep.wavやWindowsのサウンドテーマに依存しない短い二音。
// 非同期再生が終わるまでバッファが残るよう静的に保持する。
inline bool PlayIncomingSound() {
    constexpr unsigned rate = 22050, samples = rate * 42 / 100;
    static const auto wave = [] {
        std::array<unsigned char, 44 + samples * 2> bytes{};
        auto put16 = [&](unsigned offset, uint16_t value) {
            bytes[offset] = static_cast<unsigned char>(value);
            bytes[offset + 1] = static_cast<unsigned char>(value >> 8);
        };
        auto put32 = [&](unsigned offset, uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>(value >> (8 * i));
        };
        std::memcpy(bytes.data(), "RIFF", 4); put32(4, static_cast<uint32_t>(bytes.size() - 8));
        std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); put32(16, 16);
        put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2);
        put16(32, 2); put16(34, 16); std::memcpy(bytes.data() + 36, "data", 4);
        put32(40, samples * 2);
        for (unsigned i = 0; i < samples; ++i) {
            const double t = static_cast<double>(i) / rate;
            const double local = t < .18 ? t : t - .24;
            if (local < 0 || local >= .18) continue;
            const double fadeIn = local < .01 ? local / .01 : 1.0;
            const double fadeOut = local > .16 ? (.18 - local) / .02 : 1.0;
            const double frequency = t < .18 ? 880.0 : 1174.66;
            const auto value = static_cast<int16_t>(6500 * fadeIn * fadeOut * std::sin(6.28318530718 * frequency * local));
            put16(44 + i * 2, static_cast<uint16_t>(value));
        }
        return bytes;
    }();
    return PlaySoundW(reinterpret_cast<LPCWSTR>(wave.data()), nullptr,
                      SND_MEMORY | SND_ASYNC | SND_NODEFAULT | SND_NOSTOP) != FALSE;
}

inline void FlashIncomingWindow(HWND window, bool enabled) {
    if (!window || !IsWindow(window)) return;
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = window;
    info.dwFlags = enabled ? FLASHW_TRAY | FLASHW_TIMERNOFG : FLASHW_STOP;
    info.uCount = enabled ? 8 : 0;
    FlashWindowEx(&info);
}

inline HWND incomingToastWindow = nullptr;

inline void CloseIncomingToast() {
    if (incomingToastWindow) DestroyWindow(incomingToastWindow);
}

inline LRESULT CALLBACK IncomingToastProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_NCDESTROY && incomingToastWindow == window) incomingToastWindow = nullptr;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_TIMER) { DestroyWindow(window); return 0; }
    if (message == WM_LBUTTONUP) {
        const auto owner = reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (IsWindow(owner)) { ShowWindow(owner, SW_RESTORE); SetForegroundWindow(owner); }
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        auto background = CreateSolidBrush(RGB(36, 22, 52));
        FillRect(dc, &bounds, background); DeleteObject(background);
        auto border = CreateSolidBrush(RGB(233, 183, 93));
        FrameRect(dc, &bounds, border); DeleteObject(border);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(250, 240, 255));
        const auto font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Meiryo");
        const auto old = SelectObject(dc, font);
        wchar_t title[256]{}; GetWindowTextW(window, title, 256);
        InflateRect(&bounds, -16, -14);
        DrawTextW(dc, title, -1, &bounds, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old); DeleteObject(font); EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

inline void ShowIncomingToast(HWND owner, const wchar_t* message) {
    constexpr auto className = L"CCCasterIncomingToast";
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = IncomingToastProc;
    type.lpszClassName = className; type.hCursor = LoadCursor(nullptr, IDC_HAND);
    RegisterClassW(&type);
    // 短時間に複数届いても画面を通知窓で埋めない。
    CloseIncomingToast();
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const int width = 390, height = 112;
    // owned windowにするとGUIの最小化と連動して隠れるため、復帰先だけを別途保持する。
    HWND toast = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        className, message, WS_POPUP, monitor.rcWork.right - width - 16,
        monitor.rcWork.bottom - height - 16, width, height, nullptr, nullptr, type.hInstance, owner);
    incomingToastWindow = toast;
    if (toast) {
        ShowWindow(toast, SW_SHOWNOACTIVATE);
        if (!SetTimer(toast, 1, 10000, nullptr)) DestroyWindow(toast);
    }
}
} // namespace cccaster::notification
