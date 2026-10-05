#pragma once
#include <cstdint>
namespace cccaster::public_api {
// ランチャー・ゲーム・検証用実行系で共有する通信設定。
struct NetplaySettings {
    static constexpr int DefaultDelay = 2;
    // 通常枠は内部管理。平均遅延には減速、短いスパイクには最大20Fの予測で対応。
    static constexpr int DefaultRollback = 7;
    static constexpr int MaxDelay = 8;
    static constexpr int MaxRollback = 7;
    static constexpr int BurstRollback = 20;
    static constexpr int RollbackHistoryFrames = 32;
    static_assert(BurstRollback < RollbackHistoryFrames);
    static constexpr std::uint8_t WireVersion = 10;
    [[nodiscard]] static constexpr bool IsValid(int delay, int rollback) noexcept {
        return delay >= 0 && delay <= MaxDelay && rollback >= 0 && rollback <= MaxRollback;
    }
};
} // namespace cccaster::public_api
