#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace cccaster::startup_cache {
using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;
constexpr uint32_t Limit = 32 * 1024 * 1024;

class Hash {
    BCRYPT_HASH_HANDLE handle_ = nullptr;
    bool good_ = false;
public:
    Hash() {
        struct Provider {
            BCRYPT_ALG_HANDLE handle = nullptr;
            Provider() { BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
            ~Provider() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
        };
        static Provider provider;
        good_ = provider.handle && BCryptCreateHash(provider.handle, &handle_, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hash() { if (handle_) BCryptDestroyHash(handle_); }
    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;
    void Add(const void* data, size_t size) {
        good_ = good_ && size <= UINT32_MAX &&
            BCryptHashData(handle_, (PUCHAR)data, ULONG(size), 0) >= 0;
    }
    template<class T> void Value(const T& value) { Add(&value, sizeof(value)); }
    bool Finish(Digest& digest) { return good_ && BCryptFinishHash(handle_, digest.data(), digest.size(), 0) >= 0; }
};
inline std::string Hex(const Digest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (auto byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}
// バージョン・用途・入力内容・出力内容を照合。ポインターやハンドルは格納しない。
struct Header {
    uint32_t magic = 0x52534343, version = 1, kind = 0, bytes = 0;
    Digest key{}, checksum{};
};
inline bool Checksum(const Header& header, std::span<const uint8_t> data, Digest& out) {
    Hash hash;
    hash.Add(&header, offsetof(Header, checksum));
    hash.Add(data.data(), data.size());
    return hash.Finish(out);
}
class Store {
    std::filesystem::path root_;
    uint32_t kind_;
    std::filesystem::path Path(const Digest& key) const { return root_ / (Hex(key) + ".cache"); }
public:
    Store(std::filesystem::path root, uint32_t kind) : root_(std::move(root)), kind_(kind) {}
    bool Read(const Digest& key, Bytes& data) const {
        std::ifstream file(Path(key), std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto length = file.tellg();
        Header header{};
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
            header.magic != 0x52534343 || header.version != 1 || header.kind != kind_ ||
            header.key != key || header.bytes > Limit ||
            length != std::streamoff(sizeof(header) + header.bytes)) return false;
        Bytes candidate(header.bytes);
        Digest checksum{};
        if (!file.read(reinterpret_cast<char*>(candidate.data()), candidate.size()) ||
            !Checksum(header, candidate, checksum) || checksum != header.checksum) return false;
        data = std::move(candidate);
        return true;
    }
    bool Write(const Digest& key, std::span<const uint8_t> data) const {
        if (data.size() > Limit) return false;
        Header header{};
        header.kind = kind_; header.key = key; header.bytes = uint32_t(data.size());
        if (!Checksum(header, data, header.checksum)) return false;
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) return false;
        static std::atomic<unsigned> serial{0};
        auto temp = Path(key);
        temp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(serial++) + ".tmp";
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
        const bool ok = bool(file) && MoveFileExW(temp.c_str(), Path(key).c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileW(temp.c_str());
        return ok;
    }
};
}
