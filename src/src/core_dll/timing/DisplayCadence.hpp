#pragma once
#include <cstdint>
#include <numeric>

namespace cccaster::core::timer {
// 表示専用の実QPC締切。ゲームのFrameCadence・通信時計には触れない。
// 59.94/143.856Hz等も有理数のまま保持し、遅れた表示を連打しない。
class DisplayCadence {
  public:
    bool Configure(uint32_t numerator, uint32_t denominator, int64_t now) {
        if (!numerator || !denominator) return false;
        const auto divisor = std::gcd(numerator, denominator);
        numerator /= divisor; denominator /= divisor;
        if (numerator > 1000000 || denominator > 10000 ||
            uint64_t(numerator) < 20ull * denominator ||
            uint64_t(numerator) > 1000ull * denominator) return false;
        numerator_ = numerator; period_ = 60000000ll * denominator;
        epoch_ = next_ = now; slot_ = missed_ = 0;
        return true;
    }
    bool Due(int64_t now) const { return numerator_ && now >= next_; }
    int64_t Next() const { return next_; }
    uint64_t Missed() const { return missed_; }
    void Presented(int64_t now) {
        if (!Due(now)) return;
        const auto delta = now - epoch_;
        // 積のオーバーフローを避けて経過枠数を求める。
        const int64_t elapsed = (delta / period_) * numerator_ +
                                (delta % period_) * numerator_ / period_;
        if (elapsed > slot_) missed_ += elapsed - slot_;
        slot_ = elapsed + 1;
        next_ = epoch_ + (slot_ / numerator_) * period_ +
                ((slot_ % numerator_) * period_ + numerator_ - 1) / numerator_;
    }
  private:
    uint32_t numerator_ = 0;
    int64_t period_ = 0, epoch_ = 0, next_ = 0, slot_ = 0;
    uint64_t missed_ = 0;
};
}
