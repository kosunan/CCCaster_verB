#pragma once
#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/GameCompatibility.hpp"
#include <windows.h>

namespace cccaster::game_build {
inline bool ReadableLoadedRange(uintptr_t base, uintptr_t start, size_t length) {
    uintptr_t cursor = start;
    const uintptr_t end = start + length;
    if (end < start) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.AllocationBase != reinterpret_cast<void *>(base)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
// 初期化の入口で一度だけ実施。後続の個別フックはこの結果と各箇所の署名を使う。
inline bool runtimeValidated = false;
inline bool RuntimeValidated() { return runtimeValidated; }
inline game_compat::Result ValidateLoadedRuntime() {
    runtimeValidated = false;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::array<uint8_t,4096> headers{};
    SIZE_T count=0;
    game_compat::Image image;
    if (base!=game_compat::Base || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(base),
            headers.data(),headers.size(),&count) || count!=headers.size() || !image.Parse(headers,false))
        return {game_compat::Issue::Image,"pe32",uint32_t(base)};
    if (!ReadableLoadedRange(base,0x401000,0x11918f) || !ReadableLoadedRange(base,0x51b000,0x2f5ee) ||
        !ReadableLoadedRange(base,0x54b000,0x266d64))
        return {game_compat::Issue::DataLayout,"game_data",0x54b000};
    const auto result=game_compat::Check(image,[&](uint32_t rva,std::span<uint8_t> out) {
        SIZE_T bytes=0;
        return ReadableLoadedRange(base,base+rva,out.size()) &&
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(base+rva),out.data(),out.size(),&bytes) &&
            bytes==out.size();
    });
    runtimeValidated=bool(result);
    return result;
}
}
