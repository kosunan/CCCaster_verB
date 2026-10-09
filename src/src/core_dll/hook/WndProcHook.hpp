#pragma once
#include <windows.h>

namespace cccaster::game_interface {

class WndProcHook {
  public:
    static bool Initialize(HWND hwnd);
    static void Shutdown();
    // ネット対戦・オフライン対戦の戦闘画面では偶発的な終了を防ぐ。
    static bool BlocksCloseExit();
    // 入力待機側でも同じ対戦判定と、UIが消費するEscを扱う。
    static bool BlocksEscapeExit();
    // 通常フレーム境界・観戦の受信待機で自分の窓のキューを処理する。再計算中は呼ばない。
    static void PumpMessages();

  private:
    static LRESULT CALLBACK HookedWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static WNDPROC original_WndProc;
    static HWND hooked_hwnd;
};

} // namespace cccaster::game_interface
