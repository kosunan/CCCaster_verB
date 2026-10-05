#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <atomic>

namespace cccaster::game_interface::monitor_present {
void Prepare(IDirect3DDevice9* device);
bool Active();
// true: 表示周期で扱った。false: 元の経路を使用する。
bool Submit(bool skipped, const RECT* source, const RECT* destination, HWND window,
            const RGNDATA* dirty, HRESULT& result);
void Pump(int64_t remainingUs);
inline std::atomic<bool> displayChanged{true};
inline void InvalidateDisplay() { displayChanged.store(true); }
void Reset();
}
