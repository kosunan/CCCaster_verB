#pragma once
#include <algorithm>
#include "core_dll/timing/ClockProjection.hpp"
#include <cstdint>
namespace cccaster::core::timer {
// QPCの短区間補間を、WASAPIの位相へゆっくり調律する。
// 音声相関の標本ノイズを出力の段差に変えず、故障時だけQPCへ固定切替する。
class ClockContinuity {
  public:
    explicit ClockContinuity(int64_t unitsPerUs = 1) : unitsPerUs_(unitsPerUs) {}
    int64_t Read(int64_t qpc, int64_t audio) {
        if (!initialized_) {
            initialized_ = true;
            lastQpc_ = qpc;
            output_ = qpc;
        }
        const auto elapsed = std::max<int64_t>(0, qpc - lastQpc_);
        // 小数µsを保持。1µs刻みのスピン読みでも補正が切り捨てられない。
        // 秒と剰余を分け、長い休止後もelapsed*rateの積を大きくしない。
        const auto fractional = fraction_ + (elapsed % 1000000) * ratePpm_;
        output_ += elapsed + (elapsed / 1000000) * ratePpm_ + fractional / 1000000;
        fraction_ = fractional % 1000000;
        lastQpc_ = std::max(lastQpc_, qpc);

        if (usedAudio_ && audio <= 0)
            FallBack();
        if (audio <= 0 || fallback_)
            return output_;

        if (!usedAudio_) {
            usedAudio_ = true;
            offset_ = output_ - audio;
            anchorQpc_ = controlQpc_ = qpc;
            anchorAudio_ = audio;
            return output_;
        }

        // 平滑化した出力だけを監視すると音声の停止・速度異常が隠れる。
        // 元の250ms/±1%境界を生の音声時刻へ適用する。
        if (qpc - anchorQpc_ >= 250000 * unitsPerUs_) {
            const auto real = qpc - anchorQpc_;
            const auto measured = audio - anchorAudio_;
            if (measured < real - real / 100 || measured > real + real / 100)
                FallBack();
            anchorQpc_ = qpc;
            anchorAudio_ = audio;
        }
        if (fallback_ || qpc - controlQpc_ < 10000 * unitsPerUs_)
            return output_;

        controlQpc_ = qpc;
        const auto error = audio + offset_ - output_;
        // 位相誤差を約1秒かけて解消する速度(µs/s = ppm)。出力を直接動かさない。
        // 読取り回数に補正量を掛けず、実経過時間のみで積分する。
        ratePpm_ = std::clamp<int64_t>(error / unitsPerUs_, -1000, 1000);

        // 上限を超える持続的なずれを「音声へ追従中」と隠さない。
        // 瞬間標本は許容し、5ms超の残差が1秒続いた場合だけ故障扱い。
        if (error > 5000 * unitsPerUs_ || error < -5000 * unitsPerUs_) {
            if (!residualSince_)
                residualSince_ = qpc;
            else if (qpc - residualSince_ >= 1000000 * unitsPerUs_)
                FallBack();
        } else {
            residualSince_ = 0;
        }
        return output_;
    }
    ClockAnchor Anchor() const {
        return {lastQpc_, output_, fraction_, ratePpm_};
    }
    bool IsFallback() const {
        return fallback_;
    }

  private:
    const int64_t unitsPerUs_;
    void FallBack() {
        fallback_ = true;
        ratePpm_ = fraction_ = 0;
    }
    int64_t output_ = 0, lastQpc_ = 0, offset_ = 0;
    int64_t anchorQpc_ = 0, anchorAudio_ = 0, controlQpc_ = 0;
    int64_t ratePpm_ = 0, fraction_ = 0, residualSince_ = 0;
    bool initialized_ = false, usedAudio_ = false, fallback_ = false;
};
} // namespace cccaster::core::timer
