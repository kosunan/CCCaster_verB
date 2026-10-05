#pragma once
#include "shared_contracts/CheckedPatch.hpp"
#include <windows.h>
namespace cccaster::patch {
class ProcessMemory {
    HANDLE process;
    DWORD error = 0, pageSize = 0;
    bool Checked(BOOL ok) { error = ok ? ERROR_SUCCESS : GetLastError(); return ok != FALSE; }
public:
    explicit ProcessMemory(HANDLE target = GetCurrentProcess()) : process(target) {
        SYSTEM_INFO info{}; GetSystemInfo(&info); pageSize = info.dwPageSize;
    }
    uint32_t LastError() const { return error; }
    bool SinglePage(uintptr_t address, size_t size) const { return pageSize && address / pageSize == (address + size - 1) / pageSize; }
    bool Read(uintptr_t address, void *out, size_t size) {
        SIZE_T count = 0;
        if (!Checked(ReadProcessMemory(process, reinterpret_cast<void *>(address), out, size, &count))) return false;
        if (count != size) { error = ERROR_PARTIAL_COPY; return false; }
        return true;
    }
    bool Write(uintptr_t address, const void *bytes, size_t size) {
        SIZE_T count = 0;
        if (!Checked(WriteProcessMemory(process, reinterpret_cast<void *>(address), bytes, size, &count))) return false;
        if (count != size) { error = ERROR_PARTIAL_COPY; return false; }
        return true;
    }
    bool MakeWritable(uintptr_t address, size_t size, bool executable, uint32_t &old) {
        DWORD protection = 0;
        const bool ok = Checked(VirtualProtectEx(process, reinterpret_cast<void *>(address), size,
            executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &protection));
        if (ok) old = protection;
        return ok;
    }
    bool RestoreProtection(uintptr_t address, size_t size, uint32_t old) {
        DWORD ignored = 0;
        return Checked(VirtualProtectEx(process, reinterpret_cast<void *>(address), size, old, &ignored));
    }
    bool Flush(uintptr_t address, size_t size) { return Checked(FlushInstructionCache(process, reinterpret_cast<void *>(address), size)); }
};
inline Result Apply(std::span<const Spec> specs) { ProcessMemory memory; return Apply(memory, specs); }
}
