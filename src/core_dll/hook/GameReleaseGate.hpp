#pragma once
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include "shared_contracts/GameBuild.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/timing/UpdateCadence.hpp"
#include "core_dll/timing/SpinProbe.hpp"

namespace cccaster::game_interface::game_release_gate {
// Carnival140: 4333E8 -> 4BDBC0 -> D3D Present、4333F7でCS解放。
// 旧CS解放を必ず実行した後に待つ。ゲーム時計更新・WT加算は削除しない。
inline bool installed = false;
inline unsigned char replacement[6]{};
using Leave = void (WINAPI *)(CRITICAL_SECTION *);
inline Leave originalLeave = nullptr;
inline constexpr uintptr_t Site = 0x4333f7, PresentReturn = 0x4bdd16;
inline constexpr unsigned char Expected[] = {0xff,0x15,0x70,0xb0,0x51,0x00};

inline void Release() {
    using Timing = core::timer::FrameTiming;
    if (!Timing::releaseDueTicks) return;
    const auto due = Timing::releaseDueTicks;
    const auto frame = Timing::releaseFrame;
    // TLSの初期取得は最終締切前。診断中だけ最終解放時刻も従来標本へ反映する。
    auto *probe = diagnostics::SpinProbe::Enabled() && diagnostics::SpinProbe::pending
        ? &diagnostics::SpinProbe::sample : nullptr;
    Timing::releaseDueTicks = 0;
    core::timer::WasapiClock::WaitForRelease(due);
    const auto actual = platform::RealMonotonicTicks();
    // 除算・統計標本作成は次Present冒頭へ。ここでは生の実測値だけ保持。
    Timing::releasedTicks = actual;
    Timing::releaseFrame = frame;
    if (probe) probe->gameReturn = actual;
    auto &cadence = diagnostics::UpdateCadence::Get();
    if (cadence.Armed()) cadence.Capture(actual);
}
// 既存push edxのstdcall引数1個をそのまま消費する。ゲーム側の4/8byte整列にも対応。
__attribute__((stdcall, force_align_arg_pointer, noinline))
inline void LeaveAndRelease(CRITICAL_SECTION *section) {
    originalLeave(section);
    Release();
}
inline bool Write(const unsigned char *bytes) {
    DWORD protection = 0, unused = 0;
    auto *site = reinterpret_cast<void *>(Site);
    if (!VirtualProtect(site, sizeof(Expected), PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(site, bytes, sizeof(Expected));
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), site, sizeof(Expected)) != FALSE;
    const bool restored = VirtualProtect(site, sizeof(Expected), protection, &unused) != FALSE;
    if (!flushed || !restored) {
        domain::session::DebugLog("[GameReleaseGate] code protection/cache failure");
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }
    return true;
}
inline void Install() {
    if (installed || std::getenv("CCCASTER_DISABLE_GAME_RELEASE_GATE")) return;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    game_build::PeIdentity identity;
    if (base != 0x400000 || !game_build::ReadHeaders({reinterpret_cast<const uint8_t *>(base),4096}, identity) ||
        !game_build::SupportsRuntime(game_build::IdentifyHeaders(identity))) return;
    // 分岐先まで照合し、類似版や他パッチの上へ書かない。
    constexpr unsigned char context[] = {
        0xe8,0xd3,0xa7,0x08,0x00,0x8b,0x15,0x48,0x74,0x76,0x00,0x83,0xc2,0x60,0x52,
        0xff,0x15,0x70,0xb0,0x51,0x00,0xeb,0x02,0xdd,0xd8,0x83,0x3d,0x50,0xd2,0x55,0x00,0x00};
    constexpr unsigned char present[] = {0x8b,0x47,0x04,0x8b,0x08,0x8b,0x51,0x44,
        0x6a,0,0x6a,0,0x6a,0,0x6a,0,0x50,0xff,0xd2,0x8b,0x77,0x78};
    if (std::memcmp(reinterpret_cast<const void *>(0x4333e8),context,sizeof(context)) ||
        std::memcmp(reinterpret_cast<const void *>(0x4bdd03),present,sizeof(present))) {
        domain::session::DebugLog("[GameReleaseGate] signature mismatch; Present exit retained");
        return;
    }
    replacement[0] = 0xe8; replacement[5] = 0x90;
    originalLeave = *reinterpret_cast<Leave *>(0x51b070);
    const uint32_t displacement = uint32_t(reinterpret_cast<uintptr_t>(&LeaveAndRelease) - (Site + 5));
    std::memcpy(replacement + 1, &displacement, sizeof(displacement));
    installed = Write(replacement);
    domain::session::DebugLog("[GameReleaseGate] installed=%d site=%08X after=LeaveCriticalSection", int(installed), unsigned(Site));
}
inline void Remove() {
    if (!installed) return;
    if (std::memcmp(reinterpret_cast<const void *>(Site),replacement,sizeof(replacement)) || !Write(Expected)) {
        domain::session::DebugLog("[GameReleaseGate] restore failed");
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }
    installed = false;
}
}
