#pragma once
#include <atomic>
#include <algorithm>
#include <cstdint>
namespace cccaster::core::timer {
struct ClockAnchor {
    int64_t qpc = 0, time = 0, fraction = 0, ppm = 0;
    // qpc/time/fractionはすべて1/60µs単位。
    int64_t AtTicks(int64_t now) const {
        if (!qpc)
            return now;
        const auto dt = std::max<int64_t>(0, now - qpc);
        return time + dt + dt / 1000000 * ppm + (fraction + dt % 1000000 * ppm) / 1000000;
    }
    // 短い最終待機をQPCへ逆変換。整数丸めを含め最初にtargetへ達する時刻。
    // 呼出元は直前まで通常時計で待つ。速度はClockContinuityの±1000ppm。
    int64_t DeadlineTicks(int64_t target, int64_t now) const {
        const auto remaining = target - AtTicks(now);
        if (remaining <= 0) return now;
        int64_t low = now, high = now + remaining * 2 + 4;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (AtTicks(middle) < target) low = middle + 1;
            else high = middle;
        }
        return low;
    }
};
// 全フィールドをatomicにしてC++上のdata raceを避ける。32bitでもロック不要。
// 非公開側へ書いてから世代を公開する。書手が中断されても公開側は読める。
class ClockProjection {
    static_assert(std::atomic<int64_t>::is_always_lock_free);
    struct Slot {
        std::atomic<int64_t> qpc{0}, time{0}, fraction{0}, ppm{0};
    } slots_[2];
    std::atomic<uint32_t> generation_{0};

  public:
    void Publish(ClockAnchor a) {
        const auto next = generation_.load() + 1;
        auto &slot = slots_[next & 1];
        slot.qpc.store(a.qpc);
        slot.time.store(a.time);
        slot.fraction.store(a.fraction);
        slot.ppm.store(a.ppm);
        generation_.store(next);
    }
    void Read(ClockAnchor &cached) const {
        do {
            const auto first = generation_.load();
            const auto &slot = slots_[first & 1];
            const ClockAnchor value{slot.qpc.load(), slot.time.load(), slot.fraction.load(), slot.ppm.load()};
            if (first == generation_.load()) {
                cached = value;
                return;
            }
            // 通常は前回のモデルを継続。初回だけは生QPCへ切り替えず公開側を取り直す。
        } while (!cached.qpc);
    }
};
} // namespace cccaster::core::timer
