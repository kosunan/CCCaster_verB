#include "cli_launcher/network_wrapper/ConnectionHash.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
#endif

namespace cccaster::main_app::network_wrapper {

namespace {
constexpr char BASE62_CHARS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
}

std::string ConnectionHash::Base62Encode(const std::vector<uint8_t>& data) {
    // 先頭の0バイトも保持する、上限37バイトの基数変換。
    const size_t zeros = std::find_if(data.begin(), data.end(), [](uint8_t b) { return b != 0; }) - data.begin();
    std::vector<uint8_t> digits;
    for (size_t i = zeros; i < data.size(); ++i) {
        unsigned carry = data[i];
        for (auto& digit : digits) {
            carry += static_cast<unsigned>(digit) * 256;
            digit = carry % 62; carry /= 62;
        }
        while (carry) { digits.push_back(carry % 62); carry /= 62; }
    }
    std::string result(zeros, '0');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) result += BASE62_CHARS[*it];
    return result;
}

bool ConnectionHash::Base62Decode(const std::string& text, std::vector<uint8_t>& data) {
    data.clear();
    if (text.empty() || text.size() > 50) return false;
    const size_t zeros = text.find_first_not_of('0') == std::string::npos ? text.size() : text.find_first_not_of('0');
    std::vector<uint8_t> bytes;
    for (size_t i = zeros; i < text.size(); ++i) {
        const char c = text[i];
        unsigned carry;
        if (c >= '0' && c <= '9') carry = c - '0';
        else if (c >= 'A' && c <= 'Z') carry = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') carry = c - 'a' + 36;
        else return false;
        for (auto& byte : bytes) {
            carry += static_cast<unsigned>(byte) * 62;
            byte = carry & 255; carry >>= 8;
        }
        while (carry) { bytes.push_back(carry & 255); carry >>= 8; }
        if (zeros + bytes.size() > 37) return false;
    }
    if (zeros + bytes.size() > 37) return false;
    data.assign(zeros, 0);
    data.insert(data.end(), bytes.rbegin(), bytes.rend());
    return Base62Encode(data) == text;
}

uint16_t ConnectionHash::Checksum(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xffff; // CRC-16/CCITT-FALSE、誤入力検出用
    for (uint8_t byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; ++i)
            crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

// ============================================================
// Base32 (RFC 4648) エンコード/デコード （パディングなし）
// ============================================================

static constexpr char BASE32_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

std::string ConnectionHash::Base32Encode(const std::vector<uint8_t> &data) {
    std::string result;
    result.reserve((data.size() * 8 + 4) / 5);

    uint32_t buffer = 0;
    int bitsLeft = 0;

    for (uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bitsLeft += 8;
        while (bitsLeft >= 5) {
            bitsLeft -= 5;
            result += BASE32_CHARS[(buffer >> bitsLeft) & 0x1F];
        }
    }
    if (bitsLeft > 0) {
        result += BASE32_CHARS[(buffer << (5 - bitsLeft)) & 0x1F];
    }
    return result;
}

std::vector<uint8_t> ConnectionHash::Base32Decode(const std::string &encoded) {
    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 5 / 8);

    uint32_t buffer = 0;
    int bitsLeft = 0;

    for (char c : encoded) {
        int val = -1;
        if (c >= 'A' && c <= 'Z') {
            val = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            val = c - 'a';
        } else if (c >= '2' && c <= '7') {
            val = c - '2' + 26;
        }
        if (val < 0)
            continue;

        buffer = (buffer << 5) | static_cast<uint32_t>(val);
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            result.push_back(static_cast<uint8_t>((buffer >> bitsLeft) & 0xFF));
        }
    }
    return result;
}

// ============================================================
// IPv4 変換
// ============================================================

uint32_t ConnectionHash::IpToUint32(const std::string &ip) {
    struct in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) == 1) {
        return addr.s_addr;
    }
    return 0;
}

std::string ConnectionHash::Uint32ToIp(uint32_t ip) {
    struct in_addr addr{};
    addr.s_addr = ip;
    char buf[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

// ============================================================
// IPv6 変換
// ============================================================

bool ConnectionHash::IpToBytes16(const std::string &ip, uint8_t out[16]) {
    struct in6_addr addr{};
    if (inet_pton(AF_INET6, ip.c_str(), &addr) == 1) {
        std::memcpy(out, &addr, 16);
        return true;
    }
    return false;
}

std::string ConnectionHash::Bytes16ToIp(const uint8_t bytes[16]) {
    struct in6_addr addr{};
    std::memcpy(&addr, bytes, 16);
    char buf[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, &addr, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

// ============================================================
// 公開鍵プレフィックス生成 (FNV-1a → Base32 4文字)
// ============================================================

std::string ConnectionHash::GeneratePublicKey() {
    char computerName[256] = {};
    DWORD size = sizeof(computerName);
    if (!GetComputerNameA(computerName, &size)) {
        std::strcpy(computerName, "UNKNOWN");
    }

    uint32_t hash = 2166136261u;
    for (const char *p = computerName; *p; ++p) {
        hash ^= static_cast<uint32_t>(*p);
        hash *= 16777619u;
    }

    std::string key;
    key.reserve(4);
    for (int i = 3; i >= 0; --i) {
        int val = (hash >> (i * 5)) & 0x1F;
        key += BASE32_CHARS[val];
    }
    return key;
}

// ============================================================
// ローカルIPv4取得 (最初の非ループバック/非APIPA IPv4)
// ============================================================

std::string ConnectionHash::GetLocalIpv4() {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return "";
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return "";
    }

    std::string localIp;
    for (struct addrinfo *ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        if (ptr->ai_family == AF_INET) {
            struct sockaddr_in *addr = reinterpret_cast<struct sockaddr_in *>(ptr->ai_addr);
            uint32_t ipVal = ntohl(addr->sin_addr.s_addr);

            // ループバック (127.x.x.x) をスキップ
            if ((ipVal >> 24) == 127)
                continue;

            // APIPA (169.254.x.x) をスキップ
            if ((ipVal >> 16) == 0xA9FE)
                continue;

            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
                localIp = buf;
                break; // 最初に見つかったものを使用
            }
        }
    }

    freeaddrinfo(result);
    return localIp;
}

// ============================================================
// セキュリティ: タイムウィンドウ鍵ストリーム生成
// ============================================================

uint64_t ConnectionHash::GetUtcTimestamp() {
    return static_cast<uint64_t>(std::time(nullptr));
}

uint32_t ConnectionHash::GenerateSessionToken() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

std::vector<uint8_t> ConnectionHash::GenerateKeyStream(uint64_t timeWindowIndex, size_t length) {
    // FNV-1a をシードにしたストリーム鍵生成
    // タイムウィンドウインデックスから128バイトの鍵ストリームを生成
    std::vector<uint8_t> keyStream(length);

    // FNV-1a 64bit でタイムウィンドウインデックスをハッシュ
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 8; ++i) {
        h ^= (timeWindowIndex >> (i * 8)) & 0xFF;
        h *= 1099511628211ULL;
    }

    // 追加のエントロピー: 固定ソルト "CCCaster_v10_Session"
    const char *salt = "CCCaster_v10_Session_2026";
    for (const char *p = salt; *p; ++p) {
        h ^= static_cast<uint64_t>(*p);
        h *= 1099511628211ULL;
    }

    // ストリーム展開 (各バイトをFNV連鎖で生成)
    uint64_t state = h;
    for (size_t i = 0; i < length; ++i) {
        state ^= (i + 1);
        state *= 1099511628211ULL;
        keyStream[i] = static_cast<uint8_t>((state >> 24) & 0xFF);
    }

    return keyStream;
}

void ConnectionHash::XorCipher(std::vector<uint8_t> &data, const std::vector<uint8_t> &keyStream) {
    size_t len = std::min(data.size(), keyStream.size());
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= keyStream[i];
    }
}

// ============================================================
// ペイロード構築 / 解析
// ============================================================

std::vector<uint8_t> ConnectionHash::BuildPlainPayload(const std::string &ipv4, const std::string &ipv6,
                                                       uint16_t port, uint32_t token, uint64_t timestamp,
                                                       const std::string &localIpv4) {

    std::vector<uint8_t> payload;

    // flags (1 byte)
    //   bit0 = グローバルIPv4あり
    //   bit1 = IPv6あり
    //   bit2 = ローカルIPv4あり [NEW]
    uint8_t flags = 0;
    if (!ipv4.empty())
        flags |= 0x01;
    if (!ipv6.empty())
        flags |= 0x02;
    if (!localIpv4.empty())
        flags |= 0x04;
    payload.push_back(flags);

    // IPv4 (4 bytes)
    if (flags & 0x01) {
        uint32_t ipVal = IpToUint32(ipv4);
        payload.push_back(static_cast<uint8_t>((ipVal >> 0) & 0xFF));
        payload.push_back(static_cast<uint8_t>((ipVal >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>((ipVal >> 16) & 0xFF));
        payload.push_back(static_cast<uint8_t>((ipVal >> 24) & 0xFF));
    }

    // IPv6 (16 bytes)
    if (flags & 0x02) {
        uint8_t ipv6Bytes[16] = {};
        if (IpToBytes16(ipv6, ipv6Bytes)) {
            payload.insert(payload.end(), ipv6Bytes, ipv6Bytes + 16);
        } else {
            payload[0] &= ~0x02;
        }
    }

    // Port (2 bytes, big-endian)
    payload.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((port >> 0) & 0xFF));

    // Session Token (4 bytes, big-endian)
    payload.push_back(static_cast<uint8_t>((token >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((token >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((token >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((token >> 0) & 0xFF));

    // Timestamp (4 bytes, big-endian, lower 32 bits of UTC seconds)
    uint32_t ts32 = static_cast<uint32_t>(timestamp & 0xFFFFFFFF);
    payload.push_back(static_cast<uint8_t>((ts32 >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((ts32 >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((ts32 >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((ts32 >> 0) & 0xFF));

    // ローカルIPv4 (4 bytes) — flags bit2
    if (flags & 0x04) {
        uint32_t localVal = IpToUint32(localIpv4);
        payload.push_back(static_cast<uint8_t>((localVal >> 0) & 0xFF));
        payload.push_back(static_cast<uint8_t>((localVal >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>((localVal >> 16) & 0xFF));
        payload.push_back(static_cast<uint8_t>((localVal >> 24) & 0xFF));
    }

    return payload;
}

bool ConnectionHash::ParsePlainPayload(const std::vector<uint8_t> &payload, DecodedAddress &out,
                                       uint64_t &outTimestamp) {

    size_t offset = 0;

    // flags
    if (offset >= payload.size())
        return false;
    uint8_t flags = payload[offset++];
    if (!flags || (flags & ~0x07)) return false;
    const size_t expected = 11 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 16 : 0) + ((flags & 4) ? 4 : 0);
    if (payload.size() != expected) return false;

    // IPv4
    if (flags & 0x01) {
        if (offset + 4 > payload.size())
            return false;
        uint32_t ipVal = static_cast<uint32_t>(payload[offset + 0]) |
                         (static_cast<uint32_t>(payload[offset + 1]) << 8) |
                         (static_cast<uint32_t>(payload[offset + 2]) << 16) |
                         (static_cast<uint32_t>(payload[offset + 3]) << 24);
        offset += 4;
        out.ipv4 = Uint32ToIp(ipVal);
    }

    // IPv6
    if (flags & 0x02) {
        if (offset + 16 > payload.size())
            return false;
        out.ipv6 = Bytes16ToIp(&payload[offset]);
        offset += 16;
    }

    // Port (2 bytes)
    if (offset + 2 > payload.size())
        return false;
    out.port = static_cast<uint16_t>((payload[offset] << 8) | payload[offset + 1]);
    offset += 2;

    // Session Token (4 bytes)
    if (offset + 4 > payload.size())
        return false;
    out.sessionToken =
        (static_cast<uint32_t>(payload[offset]) << 24) | (static_cast<uint32_t>(payload[offset + 1]) << 16) |
        (static_cast<uint32_t>(payload[offset + 2]) << 8) | (static_cast<uint32_t>(payload[offset + 3]));
    offset += 4;

    // Timestamp (4 bytes)
    if (offset + 4 > payload.size())
        return false;
    uint32_t ts32 =
        (static_cast<uint32_t>(payload[offset]) << 24) | (static_cast<uint32_t>(payload[offset + 1]) << 16) |
        (static_cast<uint32_t>(payload[offset + 2]) << 8) | (static_cast<uint32_t>(payload[offset + 3]));
    offset += 4;
    outTimestamp = static_cast<uint64_t>(ts32);

    // ローカルIPv4 (4 bytes) — flags bit2 [オプション、後方互換]
    if (flags & 0x04) {
        if (offset + 4 > payload.size())
            return false;
        uint32_t localVal = static_cast<uint32_t>(payload[offset + 0]) |
                            (static_cast<uint32_t>(payload[offset + 1]) << 8) |
                            (static_cast<uint32_t>(payload[offset + 2]) << 16) |
                            (static_cast<uint32_t>(payload[offset + 3]) << 24);
        offset += 4;
        out.localIpv4 = Uint32ToIp(localVal);
    }

    return out.port > 0;
}

// ============================================================
// Encode: 接続情報 → 1 + Base62（既存XORペイロード + CRC16）
// ============================================================

std::string ConnectionHash::Encode(const std::string &ipv4, const std::string &ipv6, uint16_t port,
                                   const std::string &localIpv4) {
    uint64_t now = GetUtcTimestamp();
    uint32_t token = GenerateSessionToken();
    uint64_t windowIndex = now / TIME_WINDOW_SECONDS;

    // 平文ペイロード構築
    std::vector<uint8_t> payload = BuildPlainPayload(ipv4, ipv6, port, token, now, localIpv4);

    // タイムウィンドウ鍵でXOR暗号化
    auto keyStream = GenerateKeyStream(windowIndex, payload.size());
    XorCipher(payload, keyStream);

    const auto crc = Checksum(payload);
    payload.push_back(static_cast<uint8_t>(crc >> 8));
    payload.push_back(static_cast<uint8_t>(crc));
    // 1は旧Base32の先頭に現れない。記号を使わずダブルクリック全選択を維持。
    return "1" + Base62Encode(payload);
}

// ============================================================
// Decode: 新Base62 / 旧Base32 → 復号 → IPv4, IPv6, Port, Token
// ============================================================

bool ConnectionHash::Decode(const std::string &hash, DecodedAddress &out) {
    return DecodeAt(hash, out, GetUtcTimestamp());
}

bool ConnectionHash::DecodeAt(const std::string& hash, DecodedAddress& out, uint64_t now) {
    out = {};
    if (hash.empty() || hash.size() > 128) return false;
    std::vector<uint8_t> encrypted;
    std::string publicKey;
    if (hash.front() == '1') {
        if (!Base62Decode(hash.substr(1), encrypted) || encrypted.size() < 17) return false;
        const size_t n = encrypted.size();
        const uint16_t stored = (static_cast<uint16_t>(encrypted[n - 2]) << 8) | encrypted[n - 1];
        encrypted.resize(n - 2);
        if (Checksum(encrypted) != stored) return false;
    } else {
        // 大文字正規化
        std::string normalized = hash;
        for (char& c : normalized) {
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7') || c == '-')) return false;
        }

        // 公開鍵は固定4文字 + 残りがペイロード（ハイフンなし、ダブルクリック全選択対応）
        constexpr size_t PUBLIC_KEY_LENGTH = 4;
        if (normalized.length() <= PUBLIC_KEY_LENGTH) {
            return false;
        }

        publicKey = normalized.substr(0, PUBLIC_KEY_LENGTH);
        if (publicKey.find('-') != std::string::npos) return false;
        std::string payloadStr = normalized.substr(PUBLIC_KEY_LENGTH);
        if (payloadStr.empty())
            return false;

        // Base32デコード
        encrypted = Base32Decode(payloadStr);
        if (encrypted.empty())
            return false;

    }
    uint64_t currentWindow = now / TIME_WINDOW_SECONDS;
    bool expired = false;

    // 現在のタイムウィンドウで試行、失敗したら前のウィンドウでもリトライ（境界対策）
    for (int attempt = 0; attempt <= 1; ++attempt) {
        if (currentWindow < static_cast<uint64_t>(attempt)) break;
        uint64_t windowIndex = currentWindow - static_cast<uint64_t>(attempt);

        std::vector<uint8_t> payload = encrypted; // コピーして復号
        auto keyStream = GenerateKeyStream(windowIndex, payload.size());
        XorCipher(payload, keyStream);

        uint64_t timestamp = 0;
        DecodedAddress candidate;
        candidate.publicKey = publicKey;

        if (ParsePlainPayload(payload, candidate, timestamp)) {
            // 有効期限チェック: 生成時刻から6時間以内か
            uint64_t elapsed = (now >= timestamp) ? (now - timestamp) : (timestamp - now);
            if (elapsed <= EXPIRY_SECONDS) {
                out = candidate;
                out.isExpired = false;
                return true;
            } else {
                // 境界直後も前の時間窓を最後まで試す。
                expired = true;
            }
        }
    }

    out.isExpired = expired;
    return false;
}

} // namespace cccaster::main_app::network_wrapper
