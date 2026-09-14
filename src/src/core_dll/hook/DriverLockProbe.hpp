#pragma once
// 診断専用。ロック内でログ、ヒープ確保、C++ thread_localを使わない。
#include <windows.h>
#include <tlhelp32.h>
#include <MinHook.h>
#include <cstdlib>
#include <atomic>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::diagnostics::driver_lock {
struct Event {
    DWORD tid, owner, frame, kind;
    uintptr_t lock, caller;
    int64_t begin, end;
};
struct Slot { volatile LONG ready = 0; Event event{}; };
inline Slot slots[4096];
inline volatile LONG ticket = 0, dropped = 0, frame = 0;
inline DWORD guard = TLS_OUT_OF_INDEXES;
inline std::atomic<bool> enabled{false};
inline void SetFrame(DWORD value) {
    if (enabled) InterlockedExchange(&frame, value);
}
using Enter = LONG (NTAPI *)(PRTL_CRITICAL_SECTION);
using Acquire = void (NTAPI *)(PSRWLOCK);
inline Enter originalEnter = nullptr;
inline Acquire originalExclusive = nullptr, originalShared = nullptr;

inline bool Begin() {
    if (!enabled || TlsGetValue(guard)) return false;
    return TlsSetValue(guard, reinterpret_cast<void *>(1)) != FALSE;
}
inline void Publish(const Event &event) {
    if (event.end - event.begin < 6000) return; // 実測100µs以上
    const auto index = static_cast<unsigned long>(InterlockedIncrement(&ticket)) % 4096;
    auto &slot = slots[index];
    if (InterlockedCompareExchange(&slot.ready, -1, 0) != 0) {
        InterlockedIncrement(&dropped);
        return;
    }
    slot.event = event;
    InterlockedExchange(&slot.ready, 1);
}
inline LONG NTAPI OnEnter(PRTL_CRITICAL_SECTION lock) {
    const auto saved = GetLastError();
    if (!Begin()) { SetLastError(saved); return originalEnter(lock); }
    Event event{};
    event.kind = 1;
    event.tid = GetCurrentThreadId();
    // 所有権は採取直後に変わり得る。確定した待機原因と混同しない。
    event.owner = static_cast<DWORD>(reinterpret_cast<uintptr_t>(lock->OwningThread));
    event.lock = reinterpret_cast<uintptr_t>(lock);
    event.caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    event.frame = InterlockedCompareExchange(&frame, 0, 0);
    event.begin = platform::RealMonotonicTicks();
    SetLastError(saved);
    const auto result = originalEnter(lock);
    const auto after = GetLastError();
    event.end = platform::RealMonotonicTicks();
    Publish(event);
    TlsSetValue(guard, nullptr);
    SetLastError(after);
    return result;
}
template<unsigned Kind>
void NTAPI OnAcquire(PSRWLOCK lock) {
    const auto original = Kind == 2 ? originalExclusive : originalShared;
    const auto saved = GetLastError();
    if (!Begin()) { SetLastError(saved); original(lock); return; }
    Event event{};
    event.kind = Kind;
    event.tid = GetCurrentThreadId();
    event.lock = reinterpret_cast<uintptr_t>(lock);
    event.caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    event.frame = InterlockedCompareExchange(&frame, 0, 0);
    event.begin = platform::RealMonotonicTicks();
    SetLastError(saved);
    original(lock);
    const auto after = GetLastError();
    event.end = platform::RealMonotonicTicks();
    Publish(event);
    TlsSetValue(guard, nullptr);
    SetLastError(after);
}
inline void Install() {
    const char *value = std::getenv("CCCASTER_DRIVER_LOCK_PROBE");
    if (!value || value[0] != '1' || enabled) return;
    guard = TlsAlloc();
    if (guard == TLS_OUT_OF_INDEXES) return;
    platform::RealMonotonicTicks(); // 時計の初期化をフック前に完了
    auto module = GetModuleHandleW(L"ntdll.dll");
    const char *names[] = {"RtlEnterCriticalSection", "RtlAcquireSRWLockExclusive", "RtlAcquireSRWLockShared"};
    void *hooks[] = {reinterpret_cast<void *>(&OnEnter), reinterpret_cast<void *>(&OnAcquire<2>), reinterpret_cast<void *>(&OnAcquire<3>)};
    void **originals[] = {reinterpret_cast<void **>(&originalEnter), reinterpret_cast<void **>(&originalExclusive), reinterpret_cast<void **>(&originalShared)};
    for (unsigned i = 0; i != 3; ++i) {
        auto target = reinterpret_cast<void *>(GetProcAddress(module, names[i]));
        const auto create = target ? MH_CreateHook(target, hooks[i], originals[i]) : MH_ERROR_NOT_EXECUTABLE;
        const auto activate = create == MH_OK ? MH_EnableHook(target) : create;
        domain::session::DebugLog("[DriverLockHook] name=%s create=%d enable=%d", names[i], int(create), int(activate));
    }
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Module32First(snapshot, &entry)) do {
            domain::session::DebugLog("[DriverLockModule] base=%u size=%u path=%s", unsigned(reinterpret_cast<uintptr_t>(entry.modBaseAddr)), unsigned(entry.modBaseSize), entry.szExePath);
        } while (Module32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    enabled = true;
}
inline void Flush(DWORD currentFrame) {
    if (!enabled) return;
    SetFrame(currentFrame);
    const auto saved = GetLastError();
    TlsSetValue(guard, reinterpret_cast<void *>(1));
    for (auto &slot : slots) {
        if (InterlockedCompareExchange(&slot.ready, 2, 1) != 1) continue;
        const auto event = slot.event;
        InterlockedExchange(&slot.ready, 0);
        domain::session::DebugLog("[DriverLock] f=%u kind=%u tid=%u owner=%u lock=%u caller=%u begin=%lld end=%lld", event.frame, event.kind, event.tid, event.owner, unsigned(event.lock), unsigned(event.caller), event.begin, event.end);
    }
    const auto lost = InterlockedExchange(&dropped, 0);
    if (lost) domain::session::DebugLog("[DriverLockDropped] count=%ld", lost);
    TlsSetValue(guard, nullptr);
    SetLastError(saved);
}
}
