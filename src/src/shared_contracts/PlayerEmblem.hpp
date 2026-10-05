#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <algorithm>

namespace cccaster::emblem {
// 登録BMPの288画素を描画用BGRAで保持する。ファイルのヘッダーは送信しない。
inline constexpr unsigned Width = 24, Height = 12, Bytes = Width * Height * 4, ChunkBytes = 256;
inline constexpr unsigned ChunkCount = (Bytes + ChunkBytes - 1) / ChunkBytes;
inline constexpr uint64_t CompleteMask = (uint64_t(1) << ChunkCount) - 1;
inline constexpr uint32_t Magic = 0x324d4543; // CEM2: 24x12（旧64角とは区別）
inline unsigned ChunkSize(unsigned index) { return (std::min)(ChunkBytes, Bytes - index * ChunkBytes); }
struct Image {
    uint32_t id = 0;
    std::array<uint8_t, Bytes> pixels{};
};
inline uint32_t Hash(const std::array<uint8_t, Bytes>& pixels) {
    uint32_t h = 2166136261u;
    for (auto b : pixels) h = (h ^ b) * 16777619u;
    return h && h != UINT32_MAX ? h : 1;
}
inline bool Valid(const Image& image) {
    if (image.id) return image.id == Hash(image.pixels);
    for (auto b : image.pixels) if (b) return false;
    return true;
}
struct Chunk {
    uint32_t magic = Magic, id = 0, index = 0, acknowledged = UINT32_MAX;
    std::array<uint8_t, ChunkBytes> pixels{};
    bool Valid() const { return magic == Magic && index < ChunkCount && (id || index == 0); }
};
static_assert(sizeof(Chunk) == 272);
inline Chunk MakeChunk(const Image& image, unsigned index, uint32_t acknowledged = UINT32_MAX) {
    Chunk result;
    result.id = image.id; result.acknowledged = acknowledged;
    result.index = image.id ? index % ChunkCount : 0;
    if (image.id) std::memcpy(result.pixels.data(), image.pixels.data() + result.index * ChunkBytes, ChunkSize(result.index));
    return result;
}
class Receiver {
    Image pending_{};
    uint64_t received_ = 0;
    bool completed_ = false;
public:
    uint32_t Id() const { return completed_ ? pending_.id : UINT32_MAX; }
    // 完成時だけ公開。欠落・重複・並べ替え・破損はゲーム同期へ影響しない。
    std::shared_ptr<const Image> Accept(const Chunk& chunk) {
        if (!chunk.Valid()) return {};
        if (completed_) return {}; // セッション中はプロフィールを固定する。
        if (received_ && pending_.id != chunk.id) return {};
        pending_.id = chunk.id;
        if (!chunk.id) {
            pending_ = {}; completed_ = true;
            return std::make_shared<const Image>(pending_);
        }
        std::memcpy(pending_.pixels.data() + chunk.index * ChunkBytes, chunk.pixels.data(), ChunkSize(chunk.index));
        received_ |= uint64_t(1) << chunk.index;
        if (received_ != CompleteMask) return {};
        if (!Valid(pending_)) { received_ = 0; pending_ = {}; return {}; }
        completed_ = true;
        return std::make_shared<const Image>(pending_);
    }
};
class Exchange {
    std::shared_ptr<const Image> local_;
    Receiver receiver_;
    int64_t began_ = -1, last_ = -1, finished_ = -1;
    unsigned next_ = 0;
    bool acknowledged_ = false;
public:
    void Start(std::shared_ptr<const Image> local) { *this = {}; local_ = std::move(local); }
    std::shared_ptr<const Image> Receive(const Chunk& chunk) {
        if (!chunk.Valid()) return {};
        if (chunk.acknowledged == (local_ ? local_->id : 0)) acknowledged_ = true;
        return receiver_.Accept(chunk);
    }
    bool Next(int64_t nowUs, Chunk& chunk) {
        if (began_ < 0) began_ = nowUs;
        // 既存SYNC_TICKの任意末尾。画像未対応の旧相手にも終了を待たせない。
        if (nowUs - began_ > 20000000 || (last_ >= 0 && nowUs - last_ < 20000)) return false;
        if (acknowledged_ && receiver_.Id() != UINT32_MAX) {
            if (finished_ < 0) finished_ = nowUs;
            if (nowUs - finished_ > 1000000) return false;
        }
        static const Image empty;
        chunk = MakeChunk(local_ ? *local_ : empty, next_++, receiver_.Id());
        last_ = nowUs; return true;
    }
};
// 公開後の画素は不変。描画スレッドは毎F画像をコピーしない。
class Store {
    inline static std::mutex mutex_;
    inline static std::array<std::shared_ptr<const Image>, 4> images_{};
    inline static bool host_ = true;
public:
    static void Start(std::shared_ptr<const Image> local, bool host) {
        std::lock_guard lock(mutex_);
        images_ = {}; images_[0] = std::move(local); host_ = host;
    }
    static void Set(unsigned slot, std::shared_ptr<const Image> image) {
        if (slot >= images_.size()) return;
        std::lock_guard lock(mutex_); images_[slot] = std::move(image);
    }
    static std::shared_ptr<const Image> Get(unsigned slot) {
        std::lock_guard lock(mutex_); return slot < images_.size() ? images_[slot] : nullptr;
    }
    static std::array<std::shared_ptr<const Image>, 2> Players(bool spectator) {
        std::lock_guard lock(mutex_);
        if (spectator) return {images_[2], images_[3]};
        return host_ ? std::array{images_[0], images_[1]} : std::array{images_[1], images_[0]};
    }
};
}
