#pragma once
// ============================================================================
// MemDumper — Gap-tolerantマージ + memcpy最適化版
// ロールバック用メモリダンプの保存・復元
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

namespace cccaster::sync {

struct DumpEntry {
    uintptr_t addr;
    size_t size;
};

// Gap-tolerantメモリダンパー
// - 64B以内のGapを吸収してmemcpy回数を劇的に削減
// - memcpy使用（コンパイラが rep movsb / SSE2 に最適化）
// - フラットなエントリテーブル（仮想関数呼び出しゼロ）
class MemDumper {
  public:
    static constexpr size_t GAP_TOLERANCE = 64; // キャッシュライン1本分

    void SetEntries(const std::vector<DumpEntry> &entries) {
        _mergedEntries.clear();
        if (entries.empty())
            return;

        // アドレス順ソート
        auto sorted = entries;
        std::sort(sorted.begin(), sorted.end(),
                  [](const DumpEntry &a, const DumpEntry &b) { return a.addr < b.addr; });

        _mergedEntries.push_back(sorted[0]);
        for (size_t i = 1; i < sorted.size(); ++i) {
            auto &last = _mergedEntries.back();
            uintptr_t lastEnd = last.addr + last.size;
            uintptr_t nextStart = sorted[i].addr;

            if (nextStart <= lastEnd + GAP_TOLERANCE) {
                uintptr_t newEnd = std::max(lastEnd, nextStart + sorted[i].size);
                last.size = newEnd - last.addr;
            } else {
                _mergedEntries.push_back(sorted[i]);
            }
        }

        _totalCopySize = 0;
        for (auto &e : _mergedEntries)
            _totalCopySize += e.size;

        _offsets.resize(_mergedEntries.size());
        size_t offset = 0;
        for (size_t i = 0; i < _mergedEntries.size(); ++i) {
            _offsets[i] = offset;
            offset += _mergedEntries[i].size;
        }
    }

    size_t GetTotalSize() const {
        return _totalCopySize;
    }
    size_t GetEntryCount() const {
        return _mergedEntries.size();
    }

    void SaveState(char *dest) const {
        for (size_t i = 0; i < _mergedEntries.size(); ++i) {
            memcpy(dest + _offsets[i], reinterpret_cast<const void *>(_mergedEntries[i].addr),
                   _mergedEntries[i].size);
        }
    }

    void LoadState(const char *src) const {
        for (size_t i = 0; i < _mergedEntries.size(); ++i) {
            memcpy(reinterpret_cast<void *>(_mergedEntries[i].addr), src + _offsets[i],
                   _mergedEntries[i].size);
        }
    }

  private:
    std::vector<DumpEntry> _mergedEntries;
    std::vector<size_t> _offsets;
    size_t _totalCopySize = 0;
};

} // namespace cccaster::sync
