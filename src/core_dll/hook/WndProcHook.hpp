#pragma once
#include <windows.h>

namespace cccaster::game_interface {

class WndProcHook {
  public:
    static bool Initialize(HWND hwnd);
    static void Shutdown();
    // ゲームスレッドの入力待機側でも、移動取消のEscを終了操作にしない。
    static bool BlocksEscapeExit();
    // 通常フレーム境界で自分の窓のキューを処理する。再計算中は呼ばない。
    static void PumpMessages();

  private:
    static LRESULT CALLBACK HookedWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static WNDPROC original_WndProc;
    static HWND hooked_hwnd;
};

} // namespace cccaster::game_interface
