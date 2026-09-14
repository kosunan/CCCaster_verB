#pragma once
#include <cstdint>
#include <cstdlib>

namespace cccaster::diagnostics {
// ゲームスレッド専用。フック出口では整数と既存QPCの採取だけ。
// 再計算はArmしない。通常更新間に入った再計算・待機の時間は差分に残る。
class UpdateCadence {
  public:
    static constexpr int64_t Period = 1000000; // 1/60 microsecond ticks
    static constexpr int64_t Limit = 180;      // 3 microseconds
    struct Sample {
        uint32_t frame = 0, previous = 0, ordinal = 0;
        int64_t now = 0, interval = 0, error = 0;
        bool play = false, consecutive = false, spike = false;
    };
    static UpdateCadence &Get() { static UpdateCadence value; return value; }
    static bool Evidence() {
        static const bool enabled = std::getenv("CCCASTER_SPIKE_STATE_DIR") != nullptr;
        return enabled;
    }
    static bool Enabled() {
        static const bool enabled = std::getenv("CCCASTER_UPDATE_CADENCE") || Evidence();
        return enabled;
    }
    void Arm(uint32_t frame, bool play = true) { armed_ = frame; armedPlay_ = play; }
    bool Armed() const { return armed_ != 0; }
    void Capture(int64_t ticks) {
        if (!armed_) return;
        // 通常は固定値の保持だけ。前標本を取りこぼした異常時のみ先に確定する。
        if (rawPending_) Finalize();
        if (pending_) ++dropped;
        rawFrame_ = armed_; rawPlay_ = armedPlay_; rawTicks_ = ticks;
        armed_ = 0; rawPending_ = pending_ = true;
    }
    bool Take(Sample &out) {
        if (!pending_) return false;
        Finalize();
        out = sample; pending_ = false; return true;
    }
    uint32_t dropped = 0;
  private:
    void Finalize() {
        if (!rawPending_) return;
        rawPending_ = false;
        sample = {};
        sample.frame = rawFrame_; sample.previous = previousFrame_; sample.now = rawTicks_;
        sample.play = rawPlay_;
        sample.ordinal = ++count_;
        sample.consecutive = rawPlay_ && previousPlay_ && previousFrame_ && rawFrame_ == previousFrame_ + 1 &&
                             rawFrame_ / 65536 == previousFrame_ / 65536;
        if (sample.consecutive) {
            sample.interval = rawTicks_ - previousTicks_;
            sample.error = sample.interval - Period;
            sample.spike = sample.error > Limit || sample.error < -Limit;
        }
        previousTicks_ = rawTicks_; previousFrame_ = rawFrame_; previousPlay_ = rawPlay_;
    }
    uint32_t rawFrame_ = 0;
    int64_t rawTicks_ = 0;
    bool rawPlay_ = false, rawPending_ = false;
    Sample sample{};
    uint32_t armed_ = 0, previousFrame_ = 0, count_ = 0;
    int64_t previousTicks_ = 0;
    bool pending_ = false;
    bool armedPlay_ = false, previousPlay_ = false;
};
}
