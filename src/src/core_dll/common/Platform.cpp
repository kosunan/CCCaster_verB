#include "core_dll/timing/GameCoreLease.hpp"
#include "shared_contracts/IpcData.hpp"
// ============================================================================
// Platform.cpp — OS 依存処理の実装
//
// windows.hは実装ファイル内に隔離する（Platform.hppには含めない）。
// ============================================================================

#include "core_dll/common/Platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#else
#include <ctime>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#endif

#include <cstdlib>
#include <cstring>
#include <vector>
#include "core_dll/timing/GameCpuGuard.hpp"
#include "core_dll/timing/BoundaryCpuPlan.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::platform {
uint32_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return 0; // ETW突合はWindows実機のみ。
#endif
}
uint32_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}
uint32_t CurrentPhysicalCoreMask() {
#ifdef _WIN32
    GROUP_AFFINITY current{};
    if (GetActiveProcessorGroupCount() != 1 || GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) > 32 ||
        !GetThreadGroupAffinity(GetCurrentThread(), &current) || current.Group != 0 || !current.Mask) return 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> cores(
        (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (bytes && GetLogicalProcessorInformation(cores.data(), &bytes))
        for (size_t i = 0; i < bytes / sizeof(cores[0]); ++i)
            if (cores[i].Relationship == RelationProcessorCore && !(current.Mask & ~cores[i].ProcessorMask))
                return uint32_t(cores[i].ProcessorMask);
#endif
    return 0;
}
uint32_t BoundaryCpuCandidates(uint32_t gameCore) {
#ifdef _WIN32
    if (!gameCore || GetActiveProcessorGroupCount() != 1 ||
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) > 32) return 0;
    DWORD_PTR process = 0, system = 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> entries(
        (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !bytes ||
        !GetLogicalProcessorInformation(entries.data(), &bytes)) return 0;
    std::vector<uint32_t> cores;
    for (const auto &entry : entries)
        if (entry.Relationship == RelationProcessorCore) cores.push_back(uint32_t(entry.ProcessorMask));
    return core::timer::BoundaryCpuCandidates(uint32_t(process), gameCore, cores.data(), cores.size());
#else
    (void)gameCore;
    return 0;
#endif
}
TimingCpuPin::TimingCpuPin(int preferredCpu, uint32_t excludedCores) {
#ifdef _WIN32
    if (!excludedCores || preferredCpu < -1 || preferredCpu >= 32 ||
        GetActiveProcessorGroupCount() != 1 || GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) > 32)
        return;
    DWORD_PTR process = 0, system = 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> cores(
        (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !bytes ||
        !GetLogicalProcessorInformation(cores.data(), &bytes)) return;
    for (size_t i = 0; i < bytes / sizeof(cores[0]); ++i) {
        const auto &core = cores[i];
        if (core.Relationship != RelationProcessorCore || (core.ProcessorMask & (excludedCores | 1u))) continue;
        auto allowed = core.ProcessorMask & process;
        if (preferredCpu >= 0) allowed &= uintptr_t{1} << preferredCpu;
        if (!allowed) continue;
        auto lease = AcquireGameCore(uint32_t(core.ProcessorMask));
        if (!lease) continue;
        const auto single = allowed & (~allowed + 1);
        const auto previous = SetThreadAffinityMask(GetCurrentThread(), single);
        if (!previous) { ReleaseGameCore(lease); continue; }
        previous_ = previous; lease_ = lease;
        cpu_ = 0;
        while (!(single & (uintptr_t{1} << cpu_))) ++cpu_;
        break;
    }
#else
    (void)preferredCpu; (void)excludedCores;
#endif
}
TimingCpuPin::~TimingCpuPin() {
#ifdef _WIN32
    if (previous_) SetThreadAffinityMask(GetCurrentThread(), previous_);
    ReleaseGameCore(static_cast<HANDLE>(lease_));
#endif
}
TimingThread::TimingThread(const char *role) {
#ifdef _WIN32
    if (std::strcmp(role, "game") == 0) {
        DWORD_PTR processMask = 0, systemMask = 0;
        GROUP_AFFINITY current{};
        const bool disabled = std::getenv("CCCASTER_DISABLE_GAME_CPU_GUARD") != nullptr;
        DWORD bytes = 0;
        GetLogicalProcessorInformation(nullptr, &bytes);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> cores(
            (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        DWORD_PTR zeroCore = 0;
        unsigned physical = 0;
        if (bytes && GetLogicalProcessorInformation(cores.data(), &bytes)) {
            for (size_t i = 0; i < bytes / sizeof(cores[0]); ++i) {
                if (cores[i].Relationship == RelationProcessorCore) {
                    ++physical;
                    if (cores[i].ProcessorMask & 1) zeroCore = cores[i].ProcessorMask;
                }
            }
        }
        DWORD error = 0;
        uint32_t chosen = 0;
        const char *pinText = std::getenv("CCCASTER_GAME_CPU_PIN");
        int pinCpu = core::timer::ParseGameCpuPin(pinText);
        const bool autoPin = !pinText && !std::getenv("CCCASTER_DISABLE_GAME_CPU_PIN");
        uint32_t guardMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) &&
            GetThreadGroupAffinity(GetCurrentThread(), &current) && current.Group == 0) {
            // 既存のスレッド制約も維持する。CPU0だけに制限されている場合は変更せず報告。
            chosen = core::timer::GameCpuMask(static_cast<uint32_t>(processMask & current.Mask),
                static_cast<uint32_t>(zeroCore), physical,
                GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), GetActiveProcessorGroupCount());
            guardMask = chosen;
            if (!disabled && chosen) {
                chosen = core::timer::GameCpuPinMask(chosen, pinCpu);
                guardedAffinity_ = chosen;
                if (chosen != current.Mask) {
                    previousAffinity_ = SetThreadAffinityMask(GetCurrentThread(), chosen);
                    if (!previousAffinity_) error = GetLastError();
                }
            }
        } else error = GetLastError();
        // 最初はOS配置コアを優先。他のCCCasterが使用中なら別の物理コアへ。
        if (autoPin && !disabled && chosen && !error) {
            const auto currentCpu = GetCurrentProcessorNumber();
            // 選択競合を再現する診断用ヒント。許可範囲は通常と同じ。
            const int testPreference = core::timer::ParseGameCpuPin(std::getenv("CCCASTER_TEST_CPU_PREFERENCE"));
            const auto preferred = testPreference >= 0 ? unsigned(testPreference) : currentCpu;
            for (unsigned pass = 0; pass < 2 && !gameCoreLease_; ++pass) {
                for (const auto &entry : cores) {
                    if (entry.Relationship != RelationProcessorCore) continue;
                    const auto physicalMask = static_cast<uint32_t>(entry.ProcessorMask);
                    const auto allowed = physicalMask & chosen;
                    if (!allowed) continue;
                    const bool preferredCore = preferred < 32 && (physicalMask & (uint32_t{1} << preferred));
                    if ((pass == 0) != preferredCore) continue;
                    auto lease = AcquireGameCore(physicalMask);
                    if (!lease) continue;
                    const auto single = preferredCore && (allowed & (uint32_t{1} << preferred))
                        ? uint32_t{1} << preferred : allowed & (~allowed + 1);
                    const auto old = SetThreadAffinityMask(GetCurrentThread(), single);
                    if (old) {
                        if (!previousAffinity_) previousAffinity_ = old;
                        guardedAffinity_ = chosen = single;
                        gameCoreLease_ = lease;
                        pinCpu = 0;
                        while (!(single & (uint32_t{1} << pinCpu))) ++pinCpu;
                        domain::session::DebugLog("[GameCoreLease] core=%x cpu=%d acquired=1", physicalMask, pinCpu);
                        break;
                    }
                    error = GetLastError();
                    ReleaseGameCore(lease);
                }
            }
            if (!gameCoreLease_)
                domain::session::DebugLog("[GameCoreLease] acquired=0 using_guard=%x", chosen);
        }
        domain::session::DebugLog("[GameCpuGuard] tid=%lu disabled=%d process=%llx before=%llx exclude=%llx chosen=%x applied=%d error=%lu",
            GetCurrentThreadId(), int(disabled), static_cast<unsigned long long>(processMask),
            static_cast<unsigned long long>(current.Mask), static_cast<unsigned long long>(zeroCore),
            chosen, previousAffinity_ != 0, error);
        if (pinText || autoPin)
            domain::session::DebugLog("[GameCpuPin] tid=%lu cpu=%d guard=%x chosen=%x enabled=%d error=%lu auto=%d",
                GetCurrentThreadId(), pinCpu, guardMask, chosen,
                !disabled && pinCpu >= 0 && chosen == (uint32_t{1} << pinCpu) &&
                    (chosen == current.Mask || previousAffinity_ != 0), error, int(autoPin));
    }
    if (std::getenv("CCCASTER_DISABLE_MMCSS"))
        return;
    auto module = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    library_ = module;
    if (module) {
        auto join = reinterpret_cast<decltype(&AvSetMmThreadCharacteristicsW)>(
            GetProcAddress(module, "AvSetMmThreadCharacteristicsW"));
        auto priority = reinterpret_cast<decltype(&AvSetMmThreadPriority)>(
            GetProcAddress(module, "AvSetMmThreadPriority"));
        DWORD index = 0;
        if (join)
            handle_ = join(L"Games", &index);
        const bool high = handle_ && priority && priority(handle_, AVRT_PRIORITY_HIGH);
        domain::session::DebugLog("[TimingThread] role=%s tid=%lu MMCSS=%d high=%d", role,
                                  GetCurrentThreadId(), handle_ != nullptr, high);
    } else
        domain::session::DebugLog("[TimingThread] role=%s MMCSS unavailable error=%lu", role, GetLastError());
#else
    (void)role;
#endif
}
void TimingThread::MaintainAffinity() {
#ifdef _WIN32
    if (!guardedAffinity_) return;
    GROUP_AFFINITY current{};
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &current) || current.Group != 0 ||
        !(current.Mask & ~guardedAffinity_)) return;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) return;
    const auto allowed = guardedAffinity_ & processMask & current.Mask;
    if (!allowed) return; // 外部から変えたプロセス許可範囲を広げない。
    const auto previous = SetThreadAffinityMask(GetCurrentThread(), allowed);
    if (previous && !previousAffinity_) previousAffinity_ = previous;
    const auto error = previous ? 0 : GetLastError();
    if (affinityChanges_++ < 3)
        domain::session::DebugLog("[GameCpuGuardRestore] tid=%lu before=%llx chosen=%llx applied=%d error=%lu",
            GetCurrentThreadId(), static_cast<unsigned long long>(current.Mask),
            static_cast<unsigned long long>(allowed), previous != 0, error);
#endif
}
TimingThread::~TimingThread() {
#ifdef _WIN32
    ReleaseGameCore(static_cast<HANDLE>(gameCoreLease_));
    if (previousAffinity_) SetThreadAffinityMask(GetCurrentThread(), previousAffinity_);
    auto module = static_cast<HMODULE>(library_);
    if (module) {
        auto revert = reinterpret_cast<decltype(&AvRevertMmThreadCharacteristics)>(
            GetProcAddress(module, "AvRevertMmThreadCharacteristics"));
        if (handle_ && revert)
            revert(handle_);
        FreeLibrary(module);
    }
#endif
}

// ============================================================================
// 待機
// ============================================================================
void SleepMs(uint32_t ms) {
#ifdef _WIN32
    ::Sleep(static_cast<DWORD>(ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

void RealSleepMs(uint32_t ms) {
#ifdef _WIN32
    ::Sleep(static_cast<DWORD>(ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

// ============================================================================
// タイマー分解能
// ============================================================================
void BeginHighResolutionTimers() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
}

void EndHighResolutionTimers() {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

// ============================================================================
// 中断要求
// ============================================================================
#ifndef _WIN32
namespace {
std::atomic<bool> g_abortRequested{false};
void OnSignal(int sig) {
    g_abortRequested.store(true, std::memory_order_release);
    // 2回目は既定動作（即死）に戻す。フラグを立てるだけだと、
    // SceneRunner::Step() の中断チェック (H) に到達しない状態
    // （同期待ちなど）で Ctrl+C が効かなくなる。
    std::signal(sig, SIG_DFL);
}
} // namespace
#endif

bool IsAbortRequested() {
#ifdef _WIN32
    MSG message{};
    using namespace cccaster::public_api;
    if (PeekMessageA(&message, nullptr, WM_CLOSE, WM_CLOSE, PM_NOREMOVE))
        return !RequestLocalGameExit(SessionExitReason::CloseButton);
    if (PeekMessageA(&message, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE))
        return !RequestLocalGameExit(SessionExitReason::Unknown);
    if (GetAsyncKeyState(VK_F12) & 0x8000) {
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        if (foregroundPid == GetCurrentProcessId()) return !RequestLocalGameExit(SessionExitReason::F12);
    }
    return false;
#else
    return g_abortRequested.load(std::memory_order_acquire);
#endif
}

void InstallAbortHandler() {
#ifndef _WIN32
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#endif
}

// ============================================================================
// プロセス終了
// ============================================================================
void TerminateSelf() {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 1);
    // TerminateProcess は戻らないが、コンパイラにそれを教える手段がないため
    for (;;) {
    }
#else
    std::_Exit(1);
#endif
}

} // namespace cccaster::platform
