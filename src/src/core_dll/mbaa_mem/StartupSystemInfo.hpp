#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "shared_contracts/ProcessMemory.hpp"

namespace cccaster::game_memory::startup_system_info {
inline bool active = false;
inline bool dialogSkipped = false;
inline bool ChangeDialog(bool skip) {
    constexpr std::array<uint8_t, 2> original{0x6A,0x00}, skipped{0xEB,0x1C};
    // 4A3E20: INI読込み・能力検査の後から設定保存・ウィンドウ設定へ。
    // DialogBoxParamAの引数pushも一緒に飛ばし、スタックを増減させない。
    constexpr uint8_t tail[]{0x68,0x00,0x1C,0x4A,0,0x56,0x6A,0x66,0x6A,0xFA,0x56,
        0xFF,0x15,0xDC,0xB2,0x51,0,0x50,0xFF,0x15,0x74,0xB2,0x51,0,0x85,0xC0,0x74,0xD2};
    if (!startup::MatchesMenuCode() ||
        std::memcmp(reinterpret_cast<void*>(0x4A3E76), tail, sizeof(tail))) return false;
    const patch::Spec spec{"startup_settings_dialog", 0x4A3E74,
        skip ? original : skipped, skip ? skipped : original};
    const auto result = patch::Apply(std::span(&spec, 1));
    if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
    if (result) dialogSkipped = skip;
    return bool(result);
}
inline constexpr uint8_t Original[] = {0xE8,0x4C,0x02,0x00,0x00};
inline constexpr uint8_t Skipped[] = {0x90,0x90,0x90,0x90,0x90};
inline bool Change(bool skip) {
    // WM_INITDIALOGだけが呼ぶシステム情報ページ。cdeclの引数回収とxor eaxを残す。
    constexpr uint8_t prefix[] = {0x81,0x7C,0x24,0x08,0x10,0x01,0,0,0x75,0x0D,
        0x8B,0x44,0x24,0x04,0x50};
    constexpr uint8_t suffix[] = {0x83,0xC4,0x04,0x33,0xC0,0xC3};
    if (!startup::MatchesMenuCode() ||
        std::memcmp(reinterpret_cast<void*>(0x4A1F30),prefix,sizeof(prefix)) ||
        std::memcmp(reinterpret_cast<void*>(0x4A1F44),suffix,sizeof(suffix))) return false;
    auto *site = reinterpret_cast<void*>(0x4A1F3F);
    if (std::memcmp(site,skip ? Original : Skipped,5)) return false;
    DWORD protection{}, ignored{};
    if (!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&protection)) return false;
    std::memcpy(site,skip ? Skipped : Original,5);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(),site,5) != 0;
    const bool protectedAgain = VirtualProtect(site,5,protection,&ignored) != 0;
    if (!flushed || !protectedAgain) ExitProcess(ERROR_WRITE_FAULT);
    active = skip;
    return true;
}
inline void Initialize(uint8_t mode) {
    if (mode > 1 || !cccaster::diagnostics::startup::HasGate() ||
        cccaster::diagnostics::startup::Baseline() || std::getenv("CCCASTER_STARTUP_SECONDS_BASELINE")) return;
    if (!std::getenv("CCCASTER_STARTUP_MINIMAL_BASELINE") && ChangeDialog(true)) {
        domain::session::DebugLog("[StartupSystemInfo] dialogSkipped=1");
        return;
    }
    // ゲーム入口は準備イベントを待って停止中。設定読込み・他の設定ページは維持。
    cccaster::domain::session::DebugLog("[StartupSystemInfo] skip=%u",Change(true) ? 1 : 0);
}
inline void Restore() {
    if (dialogSkipped) {
        if (!ChangeDialog(false)) ExitProcess(ERROR_WRITE_FAULT);
        domain::session::DebugLog("[StartupSystemInfo] dialogRestored=1");
    }
    if (!active) return;
    // 最初のゲームフレームで復元。以後に設定画面を開いた場合は通常の情報収集。
    if (!Change(false)) ExitProcess(ERROR_WRITE_FAULT);
    cccaster::domain::session::DebugLog("[StartupSystemInfo] restored=1");
}
}
