#pragma once
#include <atomic>
#include <cstdint>
#include "shared_contracts/NetplaySettings.hpp"
namespace cccaster::core::sync {
// 確定入力列の上位8bitに設定操作を載せる。ゲームへ渡す前に除去する。
// 同一フレームの競合はP1→P2の順。新しいD/Rは次の世代入口で適用する。
struct SettingsCommands {
    inline static std::atomic<uint32_t> queued{0}, pending{0};
    inline static std::atomic<int> delay{2}, rollback{4};
    inline static std::atomic<int> notice{0}; // 1:合計超過、2:同期済み、3:境界で取消
    static constexpr uint32_t GameMask = 0x00ffffffu;
    static void Reset(int d, int r) {
        queued = 0;
        pending = 0;
        delay = d;
        rollback = r;
        notice = 0;
    }
    static bool Request(bool isRollback, int number) {
        if (number < 0 || number > 8) {
            notice = 1;
            return false;
        }
        uint32_t empty = 0, command = uint32_t((isRollback ? 0xb0 : 0xa0) | number) << 24;
        if (!pending.compare_exchange_strong(empty, command))
            return false;
        queued = command;
        notice = 0;
        return true;
    }
    static uint32_t Capture() {
        return queued.exchange(0);
    }
    static bool Apply(uint32_t input, bool local) {
        const auto command = input & ~GameMask;
        const auto code = input >> 24;
        if ((code & 0xf0) != 0xa0 && (code & 0xf0) != 0xb0)
            return false;
        int d = delay.load(), r = rollback.load();
        if ((code & 0xf0) == 0xa0)
            d = code & 15;
        else
            r = code & 15;
        const bool valid = public_api::NetplaySettings::IsValid(d, r);
        if (valid) {
            delay = d;
            rollback = r;
            notice = 2;
        } else
            notice = 1;
        if (local) {
            uint32_t expected = command;
            pending.compare_exchange_strong(expected, 0);
        }
        return true;
    }
    static void Boundary() {
        queued = 0;
        if (pending.exchange(0))
            notice = 3;
    }
};
} // namespace cccaster::core::sync
