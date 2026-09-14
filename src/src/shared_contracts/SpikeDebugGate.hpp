#pragma once
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstdlib>
namespace cccaster::diagnostics {
inline void SpikeDebugGateName(char* name, size_t length, DWORD pid) {
    std::snprintf(name, length, "Local\\CCCaster.SpikeDebugReady.%lu", pid);
}
inline void NotifySpikeDebugReady() {
    static const bool enabled = std::getenv("CCCASTER_DEBUG_SPIKES") != nullptr;
    static bool sent = false;
    if (!enabled || sent) return;
    char name[96]; SpikeDebugGateName(name, sizeof(name), GetCurrentProcessId());
    HANDLE gate=OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (gate) { sent=SetEvent(gate) != FALSE; CloseHandle(gate); }
}
}
#else
namespace cccaster::diagnostics { inline void NotifySpikeDebugReady() {} }
#endif
