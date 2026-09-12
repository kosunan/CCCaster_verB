#pragma once
#include <cstdint>
#include <algorithm>
#include "core_dll/timing/ClockUnits.hpp"
namespace cccaster::core::timer {
// 1/60µs単位。60Hzの1F = 1000000ticksを整数で正確に加算する。
class FrameCadence {
  public:
    void Reset(int64_t startUs) {
        ResetTicks(startUs * 60);
    }
    void ResetTicks(int64_t ticks) {
        next_ = ticks;
        fraction_ = 0;
        parts_ = 0;
    }
    void Shift(int64_t deltaUs) {
        next_ += deltaUs * 60;
    }
    void ShiftTicks(int64_t delta) { next_ += delta; }
    // 補正は1tickの百万分率。丸めは絶対締切へ加算する最後の一度だけ。
    void AdvanceCorrected(int64_t correctionParts, int scale = 1) {
        const int64_t denominator = ClockParts * std::max(1,scale);
        parts_ += ClockFrame * ClockParts + correctionParts;
        next_ += parts_ / denominator;
        parts_ %= denominator;
    }
    int64_t NextTicks() const {
        return next_;
    }
    int64_t NextUs() const {
        return (next_ + 59) / 60;
    }
    void Advance(int64_t intervalUs, int scale = 1) {
        fraction_ += 1000000 % (60LL * scale);
        next_ += intervalUs * 60 + fraction_ / scale;
        fraction_ %= scale;
    }

  private:
    int64_t next_ = 0, fraction_ = 0, parts_ = 0;
};
} // namespace cccaster::core::timer
