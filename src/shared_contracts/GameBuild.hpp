#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace cccaster::game_build {
enum class Edition { Unknown, Carnival140, Steam20170105 };

// 名前やタイムスタンプだけで固定アドレスを選ばない。
// codeHash はファイル上の .text の VirtualSize バイト（ロード後の再配置前）。
struct PeIdentity {
    uint16_t machine = 0, characteristics = 0;
    uint32_t timestamp = 0, imageBase = 0, imageSize = 0, entryRva = 0;
    uint32_t textRva = 0, textOffset = 0, textSize = 0;
};
inline uint16_t U16(std::span<const uint8_t> b, size_t p) {
    return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
}
inline uint32_t U32(std::span<const uint8_t> b, size_t p) {
    return uint32_t(U16(b, p)) | uint32_t(U16(b, p + 2)) << 16;
}
inline bool Fits(size_t total, size_t offset, size_t size) {
    return offset <= total && size <= total - offset;
}
inline bool ReadHeaders(std::span<const uint8_t> b, PeIdentity &out) {
    out = {};
    if (b.size() < 64 || U16(b, 0) != 0x5a4d) return false;
    const size_t nt = U32(b, 0x3c);
    if (nt < 64 || !Fits(b.size(), nt, 24) || U32(b, nt) != 0x4550) return false;
    const size_t optional = nt + 24, optionalSize = U16(b, nt + 20);
    const size_t sections = U16(b, nt + 6);
    if (optionalSize < 96 || !Fits(b.size(), optional, optionalSize) ||
        U16(b, optional) != 0x10b || !sections || sections > 96) return false;
    const size_t table = optional + optionalSize;
    if (!Fits(b.size(), table, sections * 40)) return false;
    PeIdentity p;
    p.machine = U16(b, nt + 4);
    p.timestamp = U32(b, nt + 8);
    p.entryRva = U32(b, optional + 16);
    p.imageBase = U32(b, optional + 28);
    p.imageSize = U32(b, optional + 56);
    p.characteristics = U16(b, optional + 70);
    if (!p.imageSize || p.entryRva >= p.imageSize) return false;
    for (size_t i = 0; i < sections; ++i) {
        const size_t s = table + i * 40;
        if (b[s] != '.' || b[s+1] != 't' || b[s+2] != 'e' || b[s+3] != 'x' ||
            b[s+4] != 't' || b[s+5] || b[s+6] || b[s+7]) continue;
        if (p.textSize) return false;
        p.textSize = U32(b, s + 8);
        p.textRva = U32(b, s + 12);
        p.textOffset = U32(b, s + 20);
        if (!p.textSize || p.textSize > U32(b, s + 16) ||
            !Fits(p.imageSize, p.textRva, p.textSize)) return false;
    }
    if (!p.textSize) return false;
    out = p;
    return true;
}
inline Edition IdentifyHeaders(const PeIdentity &p) {
    // ロード後はWindowsがOptionalHeader.ImageBaseも実配置へ更新する。
    if (p.machine != 0x14c || p.textRva != 0x1000)
        return Edition::Unknown;
    if (p.timestamp == 0x4fe44444 && p.imageSize == 0x3b4000 &&
        p.entryRva == 0xe3d7e && p.textSize == 0x11918f)
        return Edition::Carnival140;
    if (p.timestamp == 0x586e1804 && p.imageSize == 0xf3e000 &&
        p.entryRva == 0x130982 && p.textSize == 0x1695eb)
        return Edition::Steam20170105;
    return Edition::Unknown;
}
inline uint64_t CodeHash(std::span<const uint8_t> b) {
    uint64_t hash = 14695981039346656037ull;
    for (auto v : b) hash = (hash ^ v) * 1099511628211ull;
    return hash;
}
inline Edition IdentifyFile(std::span<const uint8_t> b) {
    PeIdentity p;
    if (!ReadHeaders(b, p) || p.imageBase != 0x400000 ||
        !Fits(b.size(), p.textOffset, p.textSize)) return Edition::Unknown;
    const auto edition = IdentifyHeaders(p);
    if (edition == Edition::Unknown) return edition;
    const auto expected = edition == Edition::Carnival140 ? 0x67dfc81b81c56fe1ull : 0x7ee17548da99a698ull;
    return CodeHash(b.subspan(p.textOffset, p.textSize)) == expected ? edition : Edition::Unknown;
}
inline const char *Name(Edition e) {
    switch (e) {
    case Edition::Carnival140: return "Carnival Phantasm 1.07 Rev.1.4.0";
    case Edition::Steam20170105: return "Steam 2017-01-05";
    default: return "Unknown / modified executable";
    }
}
// Steam版は識別・解析段階。対応表と保存範囲が未検証の間は旧版を適用しない。
inline bool SupportsRuntime(Edition e) { return e == Edition::Carnival140; }
}
