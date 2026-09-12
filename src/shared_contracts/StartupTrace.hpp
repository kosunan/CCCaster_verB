#pragma once
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cccaster::diagnostics::startup {
#ifdef _WIN32
inline void GateName(char *name, size_t size, DWORD pid) {
    std::snprintf(name, size, "Local\\CCCasterStartupReady_%lu", pid);
}
inline bool HasGate() {
    static const bool available = [] {
        char name[96]{}; GateName(name, sizeof(name), GetCurrentProcessId());
        const HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
        if (!event) return false;
        CloseHandle(event);
        return true;
    }();
    return available;
}
inline void SignalReady() {
    char name[96]{}; GateName(name, sizeof(name), GetCurrentProcessId());
    const HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (event) { SetEvent(event); CloseHandle(event); }
}
#endif
inline bool Enabled() {
    static const bool value = std::getenv("CCCASTER_STARTUP_TRACE") != nullptr;
    return value;
}
// 比較試験専用。同じ計測コードのまま従来の待機・ナビを再現する。
inline bool Baseline() {
    static const bool value = std::getenv("CCCASTER_STARTUP_BASELINE") != nullptr;
    return value;
}
inline int64_t QpcUs() {
#ifdef _WIN32
    static const int64_t frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart / frequency * 1000000 + now.QuadPart % frequency * 1000000 / frequency;
#else
    return 0;
#endif
}
}
