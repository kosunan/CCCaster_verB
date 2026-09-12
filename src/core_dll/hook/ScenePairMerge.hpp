#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <cstdlib>
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::game_interface::scene_pair_merge {
// このゲーム版の連続EndScene/BeginSceneだけを対象にする。
// ドロー・転送・状態設定やシーン外の区間は移動しない。
inline uintptr_t endCaller = 0, beginCaller = 0;
inline uintptr_t endAfterDraw = 0, beginAfterDraw = 0;
inline thread_local uintptr_t expectedBegin = 0;
inline thread_local IDirect3DDevice9 *active = nullptr, *pending = nullptr;
inline thread_local unsigned merged = 0;
inline void Initialize() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    if (std::getenv("CCCASTER_DISABLE_SCENE_MERGE")) {
        domain::session::DebugLog("[SceneMerge] disabled by environment");
        return;
    }
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != 0x4fe44444 || nt->OptionalHeader.SizeOfImage != 0x3b4000) {
        domain::session::DebugLog("[SceneMerge] game image mismatch; unchanged");
        return;
    }
    // 0x4BE349..0x4BE365 (image base 0x400000): End→Begin間は同じEBPのvtable読取りだけ。
    constexpr unsigned char expected[] = {
        0x8b,0x13,0x8b,0x6a,0x04,0x8b,0x45,0x00,0x8b,0x88,0xa8,0x00,0x00,0x00,
        0x55,0xff,0xd1,0x8b,0x55,0x00,0x8b,0x82,0xa4,0x00,0x00,0x00,0x55,0xff,0xd0};
    if (std::memcmp(reinterpret_cast<const void *>(module + 0xbe349), expected, sizeof(expected))) {
        domain::session::DebugLog("[SceneMerge] signature mismatch; unchanged");
        return;
    }
    endCaller = module + 0xbe35a;
    beginCaller = module + 0xbe366;
    // 描画直後の対。間の135バイト全体を照合し、CPUバッファ整理以外のAPIがないことを固定する。
    constexpr unsigned char afterDraw[] = {
        0x8b,0x55,0x00,0x8b,0x82,0xa8,0x00,0x00,0x00,0x55,0xff,0xd0,
        0x8b,0x43,0x04,0x8b,0x4b,0x54,0x83,0xc0,0x01,0x33,0xd2,0x83,0xf8,0x02,0x0f,0x9d,0xc2,
        0x89,0x4b,0x58,0x8d,0x73,0x5c,0x55,0x83,0xea,0x01,0x23,0xd0,0x89,0x53,0x04,0x8b,0xc2,
        0x8b,0x4c,0x83,0x0c,0x89,0x4b,0x14,0x83,0xc0,0x01,0x8d,0x14,0xc5,0x00,0x00,0x00,0x00,
        0x2b,0xd0,0x8d,0x3c,0x93,0xb9,0x07,0x00,0x00,0x00,0xf3,0xa5,0x8b,0x43,0x04,0x83,0xc0,
        0x01,0x8d,0x0c,0xc5,0x00,0x00,0x00,0x00,0x2b,0xc8,0x8b,0x43,0x14,0x8d,0x14,0x8b,0x89,
        0x53,0x54,0xc7,0x00,0x00,0x00,0x00,0x00,0xc7,0x40,0x08,0x00,0x00,0x00,0x00,0x8b,0x43,
        0x54,0x8b,0x4b,0x14,0x8b,0x50,0x08,0x89,0x51,0x10,0x8b,0x45,0x00,0x8b,0x88,0xa4,0x00,
        0x00,0x00,0xff,0xd1};
    if (!std::memcmp(reinterpret_cast<const void *>(module + 0xbe497), afterDraw, sizeof(afterDraw))) {
        endAfterDraw = module + 0xbe4a3;
        beginAfterDraw = module + 0xbe51e;
    }
    domain::session::DebugLog("[SceneMerge] verified adjacent pair end=%08X begin=%08X",
        unsigned(endCaller), unsigned(beginCaller));
    domain::session::DebugLog("[SceneMerge] after-draw pair end=%08X begin=%08X",
        unsigned(endAfterDraw), unsigned(beginAfterDraw));
}
inline bool DeferEnd(IDirect3DDevice9 *device, uintptr_t caller) {
    if (active != device || pending) return false;
    if (endCaller && caller == endCaller) expectedBegin = beginCaller;
    else if (endAfterDraw && caller == endAfterDraw) expectedBegin = beginAfterDraw;
    else return false;
    pending = device;
    return true;
}
inline bool ConsumeBegin(IDirect3DDevice9 *device, uintptr_t caller) {
    if (!pending || pending != device || caller != expectedBegin) return false;
    pending = nullptr;
    expectedBegin = 0;
    ++merged;
    return true;
}
inline void Reset() { active = pending = nullptr; expectedBegin = 0; }
} // namespace cccaster::game_interface::scene_pair_merge
