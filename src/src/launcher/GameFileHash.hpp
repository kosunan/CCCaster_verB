#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace cccaster::game_build {
// ゲーム全体をOSのSHA-256で照合する。計算失敗は起動許可にしない。
inline bool FileSha256(std::span<const uint8_t> bytes, std::string &hex) {
    hex.clear();
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) return false;
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle = nullptr;
        ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE handle = nullptr;
        ~Hash() { if (handle) BCryptDestroyHash(handle); }
    } hash;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0 ||
        (!bytes.empty() && BCryptHashData(hash.handle, const_cast<PUCHAR>(bytes.data()),
                                         static_cast<ULONG>(bytes.size()), 0) < 0) ||
        BCryptFinishHash(hash.handle, digest.data(), digest.size(), 0) < 0) return false;
    constexpr char digits[] = "0123456789abcdef";
    hex.reserve(64);
    for (const auto value : digest) {
        hex += digits[value >> 4];
        hex += digits[value & 15];
    }
    return true;
}
}
