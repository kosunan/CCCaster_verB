#pragma once
#include "shared_contracts/GameBuild.hpp"
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
// DllMainの最初に実施。別の注入器を使っても旧版の固定アドレスへ書かない。
// カニファン版専用。Steam版の実装は別プロジェクトで管理する。
inline bool ValidateLoadedRuntime() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base != 0x400000) return false;
    PeIdentity p;
    if (!ReadHeaders({reinterpret_cast<const uint8_t *>(base), 4096}, p)) return false;
    const auto edition = IdentifyHeaders(p);
    if (!SupportsRuntime(edition)) return false;
    const auto *text = reinterpret_cast<const uint8_t *>(base + p.textRva);
    if (!ReadableLoadedRange(base, base + p.textRva, p.textSize)) return false;
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
    if (hash != ExpectedCodeHash(edition)) return false;
    if (edition == Edition::Carnival140Community) {
        if (!ReadableLoadedRange(base, base + p.extraRva, p.extraSize)) return false;
        if (CodeHash({reinterpret_cast<const uint8_t *>(base + p.extraRva), p.extraSize}) !=
            CommunityExtraHash) return false;
    }
    return true;
}
}
