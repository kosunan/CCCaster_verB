#include "p2p/Crypto.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace cccaster::p2p {
namespace {
constexpr char Alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
// 新方式v1の固定識別子。旧ConnectionHashのsaltには触れない。
constexpr char Salt[] = "CCCaster_B-v1";
void Check(NTSTATUS result) {
    if (result < 0)
        throw std::runtime_error("P2P cryptography failed");
}
struct Algorithm {
    BCRYPT_ALG_HANDLE h = nullptr;
    Algorithm(LPCWSTR name, ULONG flags = 0) { Check(BCryptOpenAlgorithmProvider(&h, name, nullptr, flags)); }
    ~Algorithm() {
        if (h)
            BCryptCloseAlgorithmProvider(h, 0);
    }
};
struct Hash {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~Hash() {
        if (h)
            BCryptDestroyHash(h);
    }
};
struct AesKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~AesKey() {
        if (h)
            BCryptDestroyKey(h);
    }
};
Bytes Pbkdf(const std::string &password, std::span<const uint8_t> salt, size_t size) {
    Algorithm alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Bytes out(size);
    Check(BCryptDeriveKeyPBKDF2(alg.h, (PUCHAR)password.data(), ULONG(password.size()), (PUCHAR)salt.data(),
                                ULONG(salt.size()), 1, out.data(), ULONG(out.size()), 0));
    return out;
}
// RFC7914のSalsa20/8。各quarter roundの位置を列・行の順で構成する。
void Salsa(uint32_t *block) {
    uint32_t x[16];
    std::copy_n(block, 16, x);
    auto q = [&](int a, int b, int c, int d) {
        x[b] ^= std::rotl(x[a] + x[d], 7);
        x[c] ^= std::rotl(x[b] + x[a], 9);
        x[d] ^= std::rotl(x[c] + x[b], 13);
        x[a] ^= std::rotl(x[d] + x[c], 18);
    };
    for (int i = 0; i < 4; ++i) {
        q(0, 4, 8, 12);
        q(5, 9, 13, 1);
        q(10, 14, 2, 6);
        q(15, 3, 7, 11);
        q(0, 1, 2, 3);
        q(5, 6, 7, 4);
        q(10, 11, 8, 9);
        q(15, 12, 13, 14);
    }
    for (int i = 0; i < 16; ++i)
        block[i] += x[i];
}
void Mix(std::vector<uint32_t> &b, uint32_t r, std::vector<uint32_t> &out) {
    uint32_t x[16];
    std::copy_n(b.data() + (2 * r - 1) * 16, 16, x);
    for (uint32_t i = 0; i < 2 * r; ++i) {
        for (int j = 0; j < 16; ++j)
            x[j] ^= b[i * 16 + j];
        Salsa(x);
        std::copy_n(x, 16, out.data() + (i / 2 + (i % 2) * r) * 16);
    }
    b.swap(out);
}
std::string Topic(std::span<const uint8_t> master, const std::string &info) {
    return Hex(Hkdf(master, info)).substr(0, 32);
}
} // namespace
std::string NormalizeCode(std::string text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == 'O')
            c = '0';
        if (c == 'I' || c == 'L')
            c = '1';
        if (c == 0 || !std::strchr(Alphabet, c))
            return {};
        out += char(c);
    }
    return out.size() == 6 ? out : std::string{};
}
Bytes Random(size_t size) {
    Bytes out(size);
    Check(BCryptGenRandom(nullptr, out.data(), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return out;
}
std::string NewCode() {
    auto data = Random(6);
    std::string out;
    for (auto c : data)
        out += Alphabet[c & 31];
    return out;
}
std::string Hex(std::span<const uint8_t> bytes) {
    std::string out;
    for (auto c : bytes) {
        out += "0123456789abcdef"[c >> 4];
        out += "0123456789abcdef"[c & 15];
    }
    return out;
}
Bytes Unhex(const std::string &s) {
    if (s.size() % 2)
        return {};
    Bytes out;
    for (size_t i = 0; i < s.size(); i += 2) {
        unsigned v = 0;
        for (int j = 0; j < 2; ++j) {
            auto c = s[i + j];
            if (c >= '0' && c <= '9')
                v = v * 16 + c - '0';
            else if (c >= 'a' && c <= 'f')
                v = v * 16 + c - 'a' + 10;
            else
                return {};
        }
        out.push_back(uint8_t(v));
    }
    return out;
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    if (a.size() != b.size())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
Key Mac(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    Algorithm alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Hash hash;
    Key out{};
    Check(BCryptCreateHash(alg.h, &hash.h, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0));
    Check(BCryptHashData(hash.h, (PUCHAR)data.data(), ULONG(data.size()), 0));
    Check(BCryptFinishHash(hash.h, out.data(), ULONG(out.size()), 0));
    return out;
}
Bytes Scrypt(const std::string &password, const std::string &salt, uint32_t n, uint32_t r) {
    if (n < 2 || (n & (n - 1)) || n > 32768 || !r || r > 8)
        throw std::invalid_argument("scrypt parameters");
    auto raw = Pbkdf(password, std::span((const uint8_t *)salt.data(), salt.size()), 128 * r);
    std::vector<uint32_t> b(32 * r), out(32 * r), v(size_t(n) * 32 * r);
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = uint32_t(raw[i * 4]) | uint32_t(raw[i * 4 + 1]) << 8 | uint32_t(raw[i * 4 + 2]) << 16 |
               uint32_t(raw[i * 4 + 3]) << 24;
    for (uint32_t i = 0; i < n; ++i) {
        std::copy(b.begin(), b.end(), v.begin() + size_t(i) * b.size());
        Mix(b, r, out);
    }
    for (uint32_t i = 0; i < n; ++i) {
        auto j = b[(2 * r - 1) * 16] & (n - 1);
        for (size_t k = 0; k < b.size(); ++k)
            b[k] ^= v[size_t(j) * b.size() + k];
        Mix(b, r, out);
    }
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = uint8_t(b[i / 4] >> (8 * (i % 4)));
    auto result = Pbkdf(password, raw, 64);
    SecureZeroMemory(v.data(), v.size() * 4);
    SecureZeroMemory(raw.data(), raw.size());
    return result;
}
Key Hkdf(std::span<const uint8_t> master, const std::string &info) {
    Key salt{};
    const auto prk = Mac(salt, master);
    Bytes input(info.begin(), info.end());
    input.push_back(1);
    return Mac(prk, input);
}
std::string Base64(std::span<const uint8_t> bytes) {
    DWORD n = 0;
    CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         nullptr, &n);
    std::string out(n, '\0');
    if (!CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &n))
        return {};
    out.resize(n);
    return out;
}
Bytes Unbase64(const std::string &text) {
    if (text.size() > 4096)
        return {};
    DWORD n = 0;
    if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr,
                              nullptr))
        return {};
    Bytes out(n);
    if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, out.data(), &n, nullptr,
                              nullptr))
        return {};
    return out;
}
Keys::Keys(const std::string &code) {
    auto normalized = NormalizeCode(code);
    if (normalized.empty())
        throw std::invalid_argument("6-character connection code required");
    master = Scrypt(normalized, Salt);
    enc = Hkdf(master, "enc");
    punch = Hkdf(master, "punch");
    status = Topic(master, "status");
    request = Topic(master, "req");
}
std::string Keys::Answer(const std::string &session) const { return Topic(master, "ans" + session); }
std::string Keys::Seal(const std::string &topic, const std::string &plain) const {
    return SealMessage(enc, topic, plain);
}
bool Keys::Open(const std::string &topic, const std::string &sealed, std::string &plain) const {
    return OpenMessage(enc, topic, sealed, plain);
}
std::string SealMessage(const Key &enc, const std::string &topic, const std::string &plain) {
    if (plain.size() > 3000)
        throw std::invalid_argument("P2P message too large");
    Algorithm alg(BCRYPT_AES_ALGORITHM);
    Check(BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
    AesKey key;
    Check(BCryptGenerateSymmetricKey(alg.h, &key.h, nullptr, 0, (PUCHAR)enc.data(), 32, 0));
    auto out = Random(12);
    out.resize(12 + plain.size() + 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = out.data();
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)topic.data();
    info.cbAuthData = ULONG(topic.size());
    info.pbTag = out.data() + 12 + plain.size();
    info.cbTag = 16;
    ULONG n = 0;
    Check(BCryptEncrypt(key.h, (PUCHAR)plain.data(), ULONG(plain.size()), &info, nullptr, 0, out.data() + 12,
                        ULONG(plain.size()), &n, 0));
    return Base64(out);
}
bool OpenMessage(const Key &enc, const std::string &topic, const std::string &sealed, std::string &plain) {
    plain.clear();
    if (sealed.size() > 4096)
        return false;
    auto in = Unbase64(sealed);
    if (in.size() < 28)
        return false;
    Algorithm alg(BCRYPT_AES_ALGORITHM);
    Check(BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
    AesKey key;
    Check(BCryptGenerateSymmetricKey(alg.h, &key.h, nullptr, 0, (PUCHAR)enc.data(), 32, 0));
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = in.data();
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)topic.data();
    info.cbAuthData = ULONG(topic.size());
    info.pbTag = in.data() + in.size() - 16;
    info.cbTag = 16;
    Bytes out(in.size() - 28);
    ULONG n = 0;
    if (BCryptDecrypt(key.h, in.data() + 12, ULONG(out.size()), &info, nullptr, 0, out.data(),
                      ULONG(out.size()), &n, 0) < 0)
        return false;
    plain.assign(out.begin(), out.end());
    return true;
}
} // namespace cccaster::p2p
