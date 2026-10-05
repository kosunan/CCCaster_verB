#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cccaster::p2p {
using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 32>;
std::string NormalizeCode(std::string text);
std::string NewCode();
Bytes Random(size_t size);
std::string Hex(std::span<const uint8_t> bytes);
Bytes Unhex(const std::string &text);
Key Mac(std::span<const uint8_t> key, std::span<const uint8_t> data);
Bytes Scrypt(const std::string &password, const std::string &salt, uint32_t n = 32768, uint32_t r = 8);
Key Hkdf(std::span<const uint8_t> master, const std::string &info);
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b);
std::string Base64(std::span<const uint8_t> bytes);
Bytes Unbase64(const std::string &text);
// nonce・暗号文・認証タグだけをBase64化し、送信先topicも認証する。
std::string SealMessage(const Key &key, const std::string &topic, const std::string &plain);
bool OpenMessage(const Key &key, const std::string &topic, const std::string &sealed, std::string &plain);
struct Keys {
    Bytes master;
    Key enc{}, punch{};
    std::string status, request;
    explicit Keys(const std::string &code);
    std::string Answer(const std::string &session) const;
    std::string Seal(const std::string &topic, const std::string &plain) const;
    bool Open(const std::string &topic, const std::string &sealed, std::string &plain) const;
};
} // namespace cccaster::p2p
