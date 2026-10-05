#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <limits>

namespace cccaster::sync {
struct SnapshotNode {
    int parent;
    uintptr_t source;
    uintptr_t offset;
    size_t size;
};
// 呼出側がゲームスレッドを所有する。親を先に復元し、そのポインターから子を解決する。
class PointerSnapshot {
  public:
    bool Configure(std::span<const SnapshotNode> nodes) {
        nodes_.clear();
        offsets_.clear();
        addresses_.clear();
        chains_.clear();
        size_ = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto &n = nodes[i];
            if (n.parent < -1 || n.parent >= int(i) || !n.size || n.size > 16 * 1024 * 1024 ||
                (n.parent >= 0 && (nodes[n.parent].size < 4 || n.source > nodes[n.parent].size - 4)) ||
                n.size > 16 * 1024 * 1024 - size_) {
                nodes_.clear();
                size_ = 0;
                return false;
            }
            offsets_.push_back(size_);
            size_ += n.size;
            nodes_.push_back(n);
        }
        addresses_.resize(nodes_.size());
        chains_.resize(nodes_.size(), false);
        // 連続した4Bノード3段は、コピーした値がそのまま次のアドレスになる。
        // ゲームの0x67BDE8 + index*0x33Cの保存表にはこの形が1000組ある。
        // アドレス値はキャッシュせず、表の形だけを初期化時に判定する。
        for (size_t i = 0; i + 2 < nodes_.size(); ++i) {
            const auto &second = nodes_[i + 1];
            const auto &third = nodes_[i + 2];
            if (nodes_[i].size == 4 && second.size == 4 && third.size == 4 &&
                second.parent == int(i) && third.parent == int(i + 1) &&
                !second.source && !second.offset && !third.source && !third.offset) {
                chains_[i] = true;
                i += 2;
            }
        }
        return !nodes_.empty();
    }
    size_t Size() const {
        return size_;
    }
    bool Save(std::span<char> bytes) {
        return Copy<false>(bytes.data(), bytes.size());
    }
    bool Load(std::span<char> bytes) {
        return Copy<true>(bytes.data(), bytes.size());
    }

  private:
    // memcpyの固定長指定で非整列アドレスも扱う。復元は親を書いてから子へ進む。
    template <bool Restore> static uint32_t CopyWord(uintptr_t addr, char *bytes) {
        uint32_t value = 0;
        if constexpr (Restore) {
            if (addr) {
                std::memcpy(&value, bytes, 4);
                std::memcpy(reinterpret_cast<void *>(addr), &value, 4);
            }
        } else {
            if (addr)
                std::memcpy(&value, reinterpret_cast<const void *>(addr), 4);
            std::memcpy(bytes, &value, 4);
        }
        return value;
    }
    template <bool Restore> bool Copy(char *bytes, size_t length) {
        if (nodes_.empty() || length != size_)
            return false;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const auto &n = nodes_[i];
            uintptr_t addr = n.source;
            if (n.parent >= 0) {
                addr = addresses_[n.parent];
                if (addr) {
                    uint32_t ptr = 0;
                    std::memcpy(&ptr, reinterpret_cast<void *>(addr + n.source), 4);
                    addr = ptr ? uintptr_t(ptr) + n.offset : 0;
                }
            }
            addresses_[i] = addr;
            if (chains_[i]) {
                char *dest = bytes + offsets_[i];
                const auto second = CopyWord<Restore>(addr, dest);
                addresses_[i + 1] = second;
                const auto third = CopyWord<Restore>(second, dest + 4);
                addresses_[i + 2] = third;
                CopyWord<Restore>(third, dest + 8);
                i += 2;
                continue;
            }
            if (addr) {
                // 現行表の大半を占める4バイト項目は、可変長memcpy呼出しを避ける。
                if (n.size == 4) {
                    if constexpr (Restore)
                        std::memcpy(reinterpret_cast<void *>(addr), bytes + offsets_[i], 4);
                    else
                        std::memcpy(bytes + offsets_[i], reinterpret_cast<void *>(addr), 4);
                } else {
                    if constexpr (Restore)
                        std::memcpy(reinterpret_cast<void *>(addr), bytes + offsets_[i], n.size);
                    else
                        std::memcpy(bytes + offsets_[i], reinterpret_cast<void *>(addr), n.size);
                }
            } else if constexpr (!Restore) {
                if (n.size == 4)
                    std::memset(bytes + offsets_[i], 0, 4);
                else
                    std::memset(bytes + offsets_[i], 0, n.size);
            }
        }
        return true;
    }
    std::vector<SnapshotNode> nodes_;
    std::vector<size_t> offsets_;
    std::vector<uintptr_t> addresses_;
    std::vector<uint8_t> chains_;
    size_t size_ = 0;
};
} // namespace cccaster::sync
