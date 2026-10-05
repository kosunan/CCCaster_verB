#pragma once
#include <windows.h>
#include <d3d9.h>

namespace cccaster::game_interface::borderless {
// ウィンドウとD3D資源はゲームスレッドだけで操作する。
void ConfigureDevice(HWND window, D3DPRESENT_PARAMETERS& parameters, bool creating);
void Attach(HWND window);
void Detach();
bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
bool Active();
void SetAspectQueryAvailable(bool available);
bool RenderingClientRect(HWND window, RECT& rect);
RECT FitAspect(LONG width, LONG height, LONG sourceWidth, LONG sourceHeight);
RECT FitContent(LONG width, LONG height);
void ReleaseResources();
// trueならresultが今回の提示結果。falseなら元のPresentを使用する。
bool Present(IDirect3DDevice9* device, HRESULT& result, DWORD flags = 0);
}
