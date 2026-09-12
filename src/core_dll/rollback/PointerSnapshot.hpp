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
    size_t size_ = 0;
};
} // namespace cccaster::sync
