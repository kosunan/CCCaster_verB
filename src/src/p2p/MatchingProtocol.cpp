#include "p2p/Matching.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cccaster::matching {
namespace {
void Check(NTSTATUS status) { if (status < 0) throw std::runtime_error("Matching signature failed"); }
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(LPCWSTR name) { Check(BCryptOpenAlgorithmProvider(&handle, name, nullptr, 0)); }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
p2p::Key Digest(const std::string& text) {
    Algorithm algorithm(BCRYPT_SHA256_ALGORITHM);
    p2p::Key hash{};
    BCRYPT_HASH_HANDLE handle = nullptr;
    Check(BCryptCreateHash(algorithm.handle, &handle, nullptr, 0, nullptr, 0, 0));
    struct Cleanup { BCRYPT_HASH_HANDLE h; ~Cleanup() { BCryptDestroyHash(h); } } cleanup{handle};
    Check(BCryptHashData(handle, (PUCHAR)text.data(), ULONG(text.size()), 0));
    Check(BCryptFinishHash(handle, hash.data(), 32, 0));
    return hash;
}
bool Text(const Json& value, const char* key, size_t limit, bool empty = false) {
    if (!value.contains(key) || !value[key].is_string()) return false;
    const auto text = value[key].get<std::string>();
    return (empty || !text.empty()) && text.size() <= limit &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// 旧投稿では省略可。浮動小数・負値・int64_tを超える値を時刻として使わない。
bool Timestamp(const Json& value, const char* key) {
    if (!value.contains(key)) return true;
    const auto& time = value[key];
    return time.is_number_integer() && time > 0 &&
        (!time.is_number_unsigned() || time.get<uint64_t>() <= uint64_t(std::numeric_limits<int64_t>::max()));
}
}
const std::string& DirectoryTopic() {
    // 全端末で共有する固定値。秘密鍵ではなく、公開トピック名の直書きを避けるためのもの。
    static const auto topic = p2p::Hex(Digest("56c09078776bef7acb15f32c43beae56a3dabfbae231cfb21f8629b774da6642")).substr(0, 32);
    return topic;
}
Identity::Identity() {
    Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
    BCRYPT_KEY_HANDLE key = nullptr;
    Check(BCryptGenerateKeyPair(algorithm.handle, &key, 256, 0));
    key_ = key;
    try {
        Check(BCryptFinalizeKeyPair(key, 0));
        ULONG size = 0;
        Check(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0));
        p2p::Bytes blob(size);
        Check(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), size, &size, 0));
        publicKey = p2p::Hex(blob);
    } catch (...) { BCryptDestroyKey(key); key_ = nullptr; throw; }
}
Identity::~Identity() { if (key_) BCryptDestroyKey(key_); }
Json Identity::Sign(Json value) const {
    value["key"] = publicKey;
    value.erase("signature");
    auto hash = Digest(value.dump());
    p2p::Bytes signature(64); ULONG size = 0;
    Check(BCryptSignHash(key_, nullptr, hash.data(), ULONG(hash.size()), signature.data(), 64, &size, 0));
    value["signature"] = p2p::Hex(signature);
    return value;
}
bool Verified(const Json& value) {
    try {
        if (!value.is_object() || value.dump().size() > 3000) return false;
        auto blob = p2p::Unhex(value.at("key").get<std::string>());
        auto signature = p2p::Unhex(value.at("signature").get<std::string>());
        if (blob.size() != 72 || signature.size() != 64) return false;
        Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                               blob.data(), ULONG(blob.size()), 0) < 0) return false;
        struct Cleanup { BCRYPT_KEY_HANDLE key; ~Cleanup() { BCryptDestroyKey(key); } } cleanup{key};
        auto unsignedValue = value; unsignedValue.erase("signature");
        auto hash = Digest(unsignedValue.dump());
        return BCryptVerifySignature(key, nullptr, hash.data(), 32, signature.data(), 64, 0) >= 0;
    } catch (...) { return false; }
}
bool Registration(const Json& value) {
    try {
        return value.at("v") == 1 && value.at("kind") == "matching" &&
            value.at("id") == p2p::Hex(Digest(value.at("key").get<std::string>())) &&
            Text(value, "code", 6) && p2p::NormalizeCode(value["code"]) == value["code"].get<std::string>() &&
            Text(value, "name", 31) && Text(value, "comment", 160, true) &&
            value.at("spectators").is_boolean() && value.at("revision").is_number_unsigned() &&
            value.at("revision").get<uint64_t>() > 0 &&
            Timestamp(value, "created") && Timestamp(value, "listed_at") && Timestamp(value, "cleanup_before") &&
            (value.at("action") == "register" || value.at("action") == "remove" || value.at("action") == "closed") &&
            Verified(value);
    } catch (...) { return false; }
}
std::string ControlTopic(const p2p::Keys& keys) { return keys.Answer("matching-v1"); }
bool Directory::Apply(const Json& value, int64_t now) {
    if (!Registration(value)) return false;
    bool cleaned = false;
    if ((value["action"] == "register" || value["action"] == "remove") &&
        value.contains("cleanup_before") && now > PublicListingLifetimeSeconds) {
        // 掲載・取消の1投稿に同梱された掃除境界。受信側でも6時間未満の掲載を保護する。
        const auto cutoff = std::min(value["cleanup_before"].get<int64_t>(), now - PublicListingLifetimeSeconds);
        if (cutoff > cleanupBefore_) {
            cleanupBefore_ = cutoff;
            std::erase_if(people_, [this](const auto& entry) { return Expired(entry.second.listedAt); });
            std::erase_if(order_, [this](const auto& key) { return !people_.contains(key); });
            cleaned = true;
        }
    }
    const std::string id = value["id"];
    const auto revision = value["revision"].get<uint64_t>();
    auto prior = versions_.find(id);
    if (prior != versions_.end() && prior->second >= revision) return cleaned;
    if (prior == versions_.end() && versions_.size() >= 4096) { incomplete = true; return cleaned; }
    versions_[id] = revision;
    // 旧版の署名済みcreatedを互換利用。時刻のない投稿は推測して消さない。
    const auto listedAt = value.value("listed_at", value.value("created", int64_t(0)));
    if (value["action"] != "register" || Expired(listedAt)) {
        people_.erase(id);
        std::erase(order_, id);
        return true;
    }
    if (!people_.contains(id)) order_.push_back(id);
    const auto result = people_.contains(id) ? people_[id].result : std::string{};
    people_[id] = {id, value["code"], value["name"], value["comment"], value["key"], result,
                   value["spectators"], revision, listedAt};
    return true;
}
std::vector<Person> Directory::People() const {
    std::vector<Person> out;
    for (const auto& id : order_) if (auto found = people_.find(id); found != people_.end()) out.push_back(found->second);
    return out;
}
void Directory::Result(const std::string& id, const std::string& result) {
    if (auto found = people_.find(id); found != people_.end()) found->second.result = result;
}
// 個人登録IDも公開鍵のハッシュで束縛する。
std::string IdentityId(const Identity& identity) { return p2p::Hex(Digest(identity.publicKey)); }
} // namespace cccaster::matching
