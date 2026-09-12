#pragma once
#include "core_dll/timing/ClockUnits.hpp"
#include <algorithm>
#include <cstdint>
namespace cccaster::core::timer {
// 採用済み時計との位相差だけを10秒の時定数で戻す。1F一度、最大0.25µs/F。
// 値は論理1/60µsの百万分率。大差復帰は別経路で明示記録する。
class PhaseFollower {
  public:
    void Reset() { *this = PhaseFollower{}; }
    int64_t UpdateParts(int64_t errorTicks, uint32_t frame, bool trusted, int scale = 1) {
        if (frame == frame_) return 0;
        frame_ = frame;
        const auto error = errorTicks * std::max(1,scale);
        const auto goal = trusted ? std::clamp<int64_t>(error*ClockParts/600,-15*ClockParts,15*ClockParts) : 0;
        rate_ += std::clamp<int64_t>(goal-rate_,-ClockParts,ClockParts);
        // 反転も0を通って変化させる。失効時も最大15Fで停止する。
        return rate_;
    }
    int64_t RecoveryTicks(int64_t errorTicks, uint32_t frame, uint64_t observation, int scale = 1) {
        if (observation == observation_) return 0;
        observation_ = observation;
        const auto magnitude = std::abs(errorTicks);
        const auto direction = (errorTicks > 0) - (errorTicks < 0);
        if (magnitude < ClockFrame/std::max(1,scale)) { recoveryDirection_ = 0; return 0; }
        if (direction != recoveryDirection_) { recoveryDirection_ = direction; return 0; }
        if (recoveryFrame_ == frame) return 0;
        recoveryFrame_ = frame;
        rate_ = 0;
        return direction * std::min<int64_t>(magnitude-250*60/std::max(1,scale),
                                             100000*60/std::max(1,scale));
    }
  private:
    uint32_t frame_ = UINT32_MAX, recoveryFrame_ = UINT32_MAX;
    uint64_t observation_ = 0;
    int64_t rate_ = 0;
    int recoveryDirection_ = 0;
};
}

