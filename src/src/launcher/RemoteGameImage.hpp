#pragma once
#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/GameImageAddress.hpp"
#include <array>
#include <windows.h>

namespace cccaster::game_build {
// 32bit CREATE_SUSPENDED直後だけで使用。初期スレッドのEBXは32bit PEBを指す。
// DLL側ではGetModuleHandleW(nullptr)、起動済み観測ではモジュール列挙を使用する。
inline bool ReadSuspendedImage(HANDLE process, HANDLE thread, LoadedImage &image, PeIdentity &identity) {
    image = {}; identity = {};
    CONTEXT context{};
    context.ContextFlags = CONTEXT_INTEGER;
    if (!GetThreadContext(thread, &context) || !context.Ebx) return false;
    uint32_t base = 0;
    SIZE_T count = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void *>(uintptr_t(context.Ebx) + 8),
                           &base, sizeof(base), &count) || count != sizeof(base) || !base) return false;
    std::array<uint8_t, 4096> headers{};
    if (!ReadProcessMemory(process, reinterpret_cast<void *>(uintptr_t(base)), headers.data(),
                           headers.size(), &count) || count != headers.size() ||
        !ReadHeaders(headers, identity) || identity.machine != 0x14c) return false;
    image = {base, identity.imageSize};
    return image.Resolve(identity.entryRva, 2) != 0;
}
}
