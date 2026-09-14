#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace cccaster::game_memory::startup {
inline const double BootFadeStep = 1.0;
inline bool fadePatched = false;
// MBAACC 1.07 Rev.1.4.0の分岐と正規モード初期化入口。状態番号の直書きはしない。
inline bool MatchesMenuCode() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
    constexpr uint8_t tail[] = {0x74,0x12,0x0F,0xB6,0x05,0x0F,0xDF,0x55,0x00,0x50,
        0xE8,0xDA,0xEB,0xFF,0xFF,0xE9,0x8E,0x00,0x00,0x00};
    constexpr uint8_t training[] = {0x0F,0xB6,0x0D,0x0F,0xDF,0x55,0x00,0x51,
        0xE8,0xBA,0xEC,0xFF,0xFF,0xEB,0x71};
    constexpr uint8_t versus[] = {0x0F,0xB6,0x15,0x0F,0xDF,0x55,0x00,0x52,
        0xE8,0xED,0xEC,0xFF,0xFF,0xEB,0x54};
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(0x42B475), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize < 0x42B4E2) return false;
    return std::memcmp(reinterpret_cast<void*>(0x42B477), tail, sizeof(tail)) == 0 &&
        std::memcmp(reinterpret_cast<void*>(0x42B499), training, sizeof(training)) == 0 &&
        std::memcmp(reinterpret_cast<void*>(0x42B4B6), versus, sizeof(versus)) == 0;
}
inline bool Apply(uint8_t displacement) {
    if (displacement != 0x22 && displacement != 0x3F && displacement != 0x5C) return false;
    if (!MatchesMenuCode()) return false;
    if (displacement == 0x5C) {
        constexpr uint8_t cpu[] = {0x0F,0xB6,0x05,0x0F,0xDF,0x55,0x00,0x50,
            0xE8,0x20,0xED,0xFF,0xFF,0xEB,0x37};
        if (std::memcmp(reinterpret_cast<void*>(0x42B4D3), cpu, sizeof(cpu)) != 0) return false;
    }
    auto *code = reinterpret_cast<uint8_t*>(0x42B475);
    const uint8_t patch[] = {0xEB, displacement};
    if (std::memcmp(code, patch, 2) == 0)
        return FlushInstructionCache(GetCurrentProcess(), code, 2) != 0;
    if (code[0] != 0x84 || code[1] != 0xC0) return false;
    DWORD protection{};
    if (!VirtualProtect(code, 2, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(code, patch, 2);
    DWORD ignored{};
    const bool restored = VirtualProtect(code, 2, protection, &ignored) != 0;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), code, 2) != 0;
    if (restored && flushed && std::memcmp(code, patch, 2) == 0) return true;
    // 失敗時は元命令へ戻す。呼出側も再試行せず通常入力へ戻る。
    DWORD writable{};
    if (!VirtualProtect(code, 2, PAGE_EXECUTE_READWRITE, &writable))
        ExitProcess(ERROR_WRITE_FAULT);
    const uint8_t original[] = {0x84, 0xC0};
    std::memcpy(code, original, 2);
    const bool rollbackFlushed = FlushInstructionCache(GetCurrentProcess(), code, 2) != 0;
    const bool rollbackProtected = VirtualProtect(code, 2, protection, &ignored) != 0;
    if (!rollbackFlushed || !rollbackProtected || std::memcmp(code, original, 2) != 0)
        ExitProcess(ERROR_WRITE_FAULT);
    return false;
}
// 起動メニューstate3の暗転加算だけを変更する。共有定数・状態遷移・解放は維持。
// 呼出しと復元はゲームスレッド限定。
inline bool SetBootFade(bool enable) {
    if (fadePatched == enable) return true;
    if (!MatchesMenuCode()) return false;
    constexpr uint8_t prefix[] = {0x8B,0x86,0xC4,0,0,0,0xC7,0,2,0,0,0,
        0x89,0x58,4,0xD9,0x86,0xC0,0,0,0,0xDC,5};
    constexpr uint8_t suffix[] = {0xD9,0x5C,0x24,0x14,0xD9,0x44,0x24,0x14,
        0xD9,0x96,0xC0,0,0,0,0xD9,0xE8,0xD8,0xD1,0xDF,0xE0,0xDD,0xD9};
    if (std::memcmp(reinterpret_cast<void*>(0x42B33F), prefix, sizeof(prefix)) ||
        std::memcmp(reinterpret_cast<void*>(0x42B35A), suffix, sizeof(suffix))) return false;
    const uint32_t fast = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BootFadeStep));
    const uint32_t expected = enable ? 0x53D6B8 : fast;
    const uint32_t replacement = enable ? fast : 0x53D6B8;
    auto *operand = reinterpret_cast<void*>(0x42B356);
    uint32_t current{};
    std::memcpy(&current, operand, 4);
    if (current != expected) return false;
    DWORD protection{}, ignored{};
    if (!VirtualProtect(operand, 4, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(operand, &replacement, 4);
    if (!FlushInstructionCache(GetCurrentProcess(), operand, 4) ||
        !VirtualProtect(operand, 4, protection, &ignored)) {
        // 不完全な命令変更を残してゲームを進めない。
        ExitProcess(ERROR_WRITE_FAULT);
    }
    fadePatched = enable;
    return true;
}
}
