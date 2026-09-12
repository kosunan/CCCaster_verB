#pragma once
#include <cstdint>
namespace cccaster::public_api {
// ランチャー・ゲーム・検証用実行系で共有する通信設定。
struct NetplaySettings {
    static constexpr int DefaultDelay = 2;
    static constexpr int DefaultRollback = 4;
    static constexpr int MaxBufferedFrames = 8;
    static constexpr std::uint8_t WireVersion = 10;
    [[nodiscard]] static constexpr bool IsValid(int delay, int rollback) noexcept {
        return delay >= 0 && delay <= MaxBufferedFrames && rollback >= 0 && rollback <= MaxBufferedFrames &&
               delay + rollback <= MaxBufferedFrames;
    }
};
} // namespace cccaster::public_api
