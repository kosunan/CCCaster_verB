#include "core_dll/mbaa_mem/NativeFpsCounter.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace {
bool (*replayQuery)() = nullptr;
}
extern "C" {
void* cc_native_fps_original = nullptr;
uintptr_t cc_native_fps_without_increment = 0x41FCF6;
__attribute__((force_align_arg_pointer)) int cc_native_fps_replay(uintptr_t state) {
    return state == 0x774A60 && replayQuery && replayQuery();
}
// 41FCF0: FLD [ECX]; ADD [ECX+8],1. Replay omits only ADD, then rejoins
// the native elapsed/cost accumulation and one-second reporting window.
// Preserve the game's x87 stack, SSE state, registers and flags across C++.
__attribute__((naked)) void cc_native_fps_hook() {
    __asm__ __volatile__(
        "pushfl; pushal; movl %esp,%esi; subl $528,%esp; andl $-16,%esp; fxsave (%esp); "
        "subl $12,%esp; pushl %ecx; call _cc_native_fps_replay; addl $16,%esp; testl %eax,%eax; jz 1f; "
        "fxrstor (%esp); movl %esi,%esp; popal; popfl; flds (%ecx); jmp *_cc_native_fps_without_increment; "
        "1: fxrstor (%esp); movl %esi,%esp; popal; popfl; jmp *_cc_native_fps_original;");
}
}

namespace cccaster::game_interface::native_fps_counter {
bool Install(bool (*isReplay)()) {
    static bool installed = false;
    if (installed) return true;
    if (!isReplay || !game_build::RuntimeValidated()) return false;
    constexpr uint8_t expected[]{0xD9,0x01,0x83,0x41,0x08,0x01,0xD9,0x44,0x24,0x04,0x8B,0x51,0x08};
    auto* site = reinterpret_cast<void*>(0x41FCF0);
    if (std::memcmp(site,expected,sizeof(expected))) return false;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    replayQuery = isReplay;
    if (MH_CreateHook(site,reinterpret_cast<void*>(cc_native_fps_hook),&cc_native_fps_original) != MH_OK)
        return false;
    if (MH_EnableHook(site) != MH_OK) {
        MH_RemoveHook(site);
        return false;
    }
    installed = true;
    return true;
}
}
