#pragma once
#include "shared_contracts/GameBuild.hpp"
#include <windows.h>

namespace cccaster::game_build {
// DllMainの最初に実施。別の注入器を使っても旧版の固定アドレスへ書かない。
// カニファン版専用。Steam版の実装は別プロジェクトで管理する。
inline bool ValidateLoadedRuntime() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base != 0x400000) return false;
    PeIdentity p;
    if (!ReadHeaders({reinterpret_cast<const uint8_t *>(base), 4096}, p)) return false;
    if (IdentifyHeaders(p) != Edition::Carnival140) return false;
    const auto *text = reinterpret_cast<const uint8_t *>(base + p.textRva);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(text);
    const uintptr_t end = cursor + p.textSize;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.AllocationBase != reinterpret_cast<void *>(base)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    // ランチャーが入口だけをEB FEへ変更している期間も、その他すべてを照合する。
    const size_t entry = p.entryRva - p.textRva;
    const bool locked = text[entry] == 0xeb && text[entry + 1] == 0xfe;
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < p.textSize; ++i) {
        uint8_t value = text[i];
        if (locked && i == entry) value = 0xe8;
        if (locked && i == entry + 1) value = 0xaf;
        hash = (hash ^ value) * 1099511628211ull;
    }
    return hash == 0x67dfc81b81c56fe1ull;
}
}
