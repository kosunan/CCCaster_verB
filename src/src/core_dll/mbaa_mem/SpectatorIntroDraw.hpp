#pragma once
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <MinHook.h>
#include <cstring>

namespace cccaster::game_memory::spectator_intro_draw {
using Draw = void (__cdecl *)();
inline Draw original = nullptr;
inline bool installed = false;
inline unsigned covered = 0;

// 0x472B77から呼ばれるロード画像のタイル状フェード描画だけを除く。
// 558600のフェード値、キャラ更新、RNGには触れない。復元後の確定再生でも
// 同じ描画方針を保ち、待機解除時にロード画像が再び重なるのを防ぐ。
__attribute__((force_align_arg_pointer)) inline void __cdecl DrawCover() {
    if (*CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME && *CC_INTRO_STATE_ADDR == 2) {
        if (!covered++)
            domain::session::DebugLog("[SpectatorIntroDraw] loading_cover_hidden=1");
        return;
    }
    original();
}
// 観戦の先頭画像を準備するときだけ設置する。対戦者には設置しない。
inline bool Prepare() {
    covered = 0;
    if (installed) return true;
    constexpr unsigned char expected[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x83,0xEC,0x30};
    auto address = reinterpret_cast<void *>(0x417EA0);
    if (!game_build::RuntimeValidated() ||
        std::memcmp(address, expected, sizeof(expected))) return false;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(address, reinterpret_cast<void *>(DrawCover),
                      reinterpret_cast<void **>(&original)) != MH_OK) return false;
    if (MH_EnableHook(address) != MH_OK) {
        MH_RemoveHook(address);
        original = nullptr;
        return false;
    }
    installed = true;
    return true;
}
}
