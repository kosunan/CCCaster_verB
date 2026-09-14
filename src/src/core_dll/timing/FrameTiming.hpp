#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace cccaster::core::timer {
// ゲームスレッド専用。実QPCで提示要求間隔を測る。再計算をゲーム進行に数えない。
class FrameTiming {
  public:
    static FrameTiming &Get() {
        static FrameTiming value;
        return value;
    }
    static FrameTiming &Simulation() {
        static FrameTiming value;
        return value;
    }
    // 満タン演出の初回資源準備で観測した約4.7msを提示締切内へ収める。
    // 更新・入力の締切は動かさず、提示位相のみを確保する。試験時は旧1200と比較できる。
    static int64_t PresentBudgetUs() {
        static const int64_t budget = []() -> int64_t {
            const char *text = std::getenv("CCCASTER_PRESENT_BUDGET_US");
            if (text) {
                char *end = nullptr;
                const long parsed = std::strtol(text, &end, 10);
                if (end != text && *end == '\0' && parsed >= 0 && parsed <= 8000)
                    return parsed;
            }
            return 5000;
        }();
        return budget;
    }
    // 入力採取予定から通常更新まで3msの準備枠を取り、最後の3msはスピンする。
    static constexpr int64_t SimulationGuardUs = 3000;
    static constexpr int64_t SimulationSpinGuardUs = 3000;
    // 表示側は従来の起床余裕を維持。
    static constexpr int64_t PresentSpinGuardUs = 1000;
    inline static int64_t releaseUs = 0, workUs = 0;
    // SceneRunnerが通常更新にだけ設定するWASAPI絶対時刻。Presentで1回消費する。
    inline static int64_t presentDueTicks = 0;
    // 入力反映・記録を締切前に済ませ、Presentフック出口で残りを待つ。
    static constexpr int64_t ReleasePreparationUs = 200;
    inline static int64_t releaseDueTicks = 0;
    inline static uint32_t releaseFrame = 0;
    inline static int64_t releasedTicks = 0;
    void Reset() {
        *this = FrameTiming{};
    }
    // 締切直後は採取時刻だけ保持する。次の通常/省略Presentで次回採取より前に集計する。
    // ゲームスレッド専用の1標本スロット。Resetで世代境界の未集計標本も破棄する。
    void Capture(int64_t now, uint32_t frame, bool skipped) {
        capturedUs_ = now;
        capturedFrame_ = frame;
        capturedSkipped_ = skipped;
        captured_ = true;
    }
    bool ObserveCaptured() {
        if (!captured_)
            return false;
        captured_ = false;
        return Observe(capturedUs_, capturedFrame_, capturedSkipped_);
    }
    bool Observe(int64_t now, uint32_t frame, bool skipped) {
        if (previousFrame_ && (frame < previousFrame_ || frame - previousFrame_ > 600))
            Reset();
        previousFrame_ = frame;
        if (skipped) {
            ++skips_;
            return false;
        }
        if (!lastUs_) {
            lastUs_ = now;
            lastFrame_ = frame;
            return false;
        }
        last = now - lastUs_;
        const uint32_t advanced = frame - lastFrame_;
        lastUs_ = now;
        lastFrame_ = frame;
        sum_ -= samples_[index_];
        frames_ -= advances_[index_];
        samples_[index_] = last;
        advances_[index_] = advanced;
        sum_ += last;
        frames_ += advanced;
        index_ = (index_ + 1) % samples_.size();
        count_ = std::min(count_ + 1, samples_.size());
        const auto range = std::minmax_element(samples_.begin(), samples_.begin() + count_);
        minimum = *range.first;
        maximum = *range.second;
        displayFps = sum_ > 0 ? count_ * 1000000.0 / sum_ : 0;
        gameFps = sum_ > 0 ? frames_ * 1000000.0 / sum_ : 0;
        return ++total_ % 60 == 0;
    }
    int64_t last = 0, minimum = 0, maximum = 0;
    double displayFps = 0, gameFps = 0;
    uint32_t Skips() const {
        return skips_;
    }

  private:
    std::array<int64_t, 120> samples_{};
    std::array<uint32_t, 120> advances_{};
    size_t index_ = 0, count_ = 0;
    int64_t lastUs_ = 0, sum_ = 0;
    uint32_t lastFrame_ = 0, previousFrame_ = 0, frames_ = 0, total_ = 0, skips_ = 0;
    int64_t capturedUs_ = 0;
    uint32_t capturedFrame_ = 0;
    bool capturedSkipped_ = false, captured_ = false;
};
} // namespace cccaster::core::timer
