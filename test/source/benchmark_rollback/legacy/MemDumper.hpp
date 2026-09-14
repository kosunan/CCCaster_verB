#pragma once
// ============================================================================
// Legacy MemDumper — 旧DllRollbackManagerのSave/Loadロジックを
// ゲーム非依存に切り出したもの。性能ベースライン用。
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace legacy {

// メモリダンプの1エントリ（アドレス + サイズ）
struct DumpEntry {
    uintptr_t addr;  // ソースアドレス
    size_t    size;  // バイト数
};

// 旧実装互換のメモリダンパー
// - std::copy によるバイト単位コピー
// - エントリごとのループ（関数呼び出しシミュレーション）
class MemDumper {
public:
    void SetEntries(const std::vector<DumpEntry>& entries) {
        _entries = entries;
        _totalSize = 0;
        for (auto& e : _entries)
            _totalSize += e.size;
    }

    size_t GetTotalSize() const { return _totalSize; }
    size_t GetEntryCount() const { return _entries.size(); }

    // 旧実装: std::copy ベース (MemDumpBase::saveDump 相当)
    void SaveState(char* dest) const {
        char* dump = dest;
        for (const auto& entry : _entries) {
            const char* src = reinterpret_cast<const char*>(entry.addr);
            // std::copy はバイト単位ループ（旧実装と同一）
            std::copy(src, src + entry.size, dump);
            dump += entry.size;
        }
    }

    // 旧実装: std::copy ベース (MemDumpBase::loadDump 相当)
    void LoadState(const char* src) const {
        const char* dump = src;
        for (const auto& entry : _entries) {
            char* dst = reinterpret_cast<char*>(entry.addr);
            std::copy(dump, dump + entry.size, dst);
            dump += entry.size;
        }
    }

private:
    std::vector<DumpEntry> _entries;
    size_t _totalSize = 0;
};

} // namespace legacy
