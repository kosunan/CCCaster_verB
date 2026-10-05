#pragma once
#include "core_dll/timing/ClockUnits.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
namespace cccaster::core::timer {
// 通信スレッド専有。最新RTTの約2秒の算術平均を固定8バケットで保持する。
// 時計推定用の最小RTTとは別。受信のない期間を遅延ゼロとして平均しない。
class MeanNetworkDelay {
  public:
    void Reset() { *this = MeanNetworkDelay{}; }
    int64_t Add(int64_t now, int64_t rttUs) {
        if (rttUs <= 0 || rttUs > 6000000 || now <= 0) return Mean();
        const auto epoch = now / (ClockSecond / 4);
        if (!started_ || epoch < epoch_) { Reset(); started_ = now; epoch_ = epoch; }
        const auto elapsed = std::min<int64_t>(8, epoch - epoch_);
        for (int64_t i = 1; i <= elapsed; ++i) {
            auto &old = buckets_[(epoch_ + i) % buckets_.size()];
            sum_ -= old.sum; count_ -= old.count; old = {};
        }
        epoch_ = epoch;
        auto &bucket = buckets_[epoch % buckets_.size()];
        bucket.sum += rttUs; ++bucket.count;
        sum_ += rttUs; ++count_;
        ready_ = now - started_ >= ClockSecond / 2;
        return Mean();
    }
    int64_t Mean() const { return ready_ && count_ ? sum_ / count_ : 0; }
  private:
    struct Bucket { int64_t sum = 0; uint32_t count = 0; };
    std::array<Bucket, 8> buckets_{};
    int64_t started_ = 0, epoch_ = 0, sum_ = 0;
    uint32_t count_ = 0;
    bool ready_ = false;
};
// 入力スレッド専有。平均片道遅延がD+通常Rの時間幅を継続して超えると減速を要求。
// 単発の予測枠不足では減速しない。割当て・待機・ゲームメモリ操作は行わない。
class NetworkPacing {
  public:
    static constexpr uint32_t Normal = ClockFrame;
    static constexpr uint32_t Maximum = 100000 * 60;
    static bool Valid(uint32_t ticks) { return ticks >= Normal && ticks <= Maximum; }
    void Reset() { *this = NetworkPacing{}; }
    uint32_t Update(int64_t now, int64_t meanRttUs, int delay) {
        if (meanRttUs <= 0) return requested_;
        const int budget = std::max(1, delay + public_api::NetplaySettings::DefaultRollback);
        // RTT/2を1/60µsへ換算。0.25ms単位で切上げ、微小な平均変動を吸収する。
        constexpr int64_t quantum = 250 * 60;
        const auto required = (std::min<int64_t>(meanRttUs, 6000000) * 30 + budget - 1) / budget;
        const auto target = required <= Normal ? Normal : uint32_t(std::clamp<int64_t>(
            ((required + quantum - 1) / quantum) * quantum, Normal, Maximum));
        if (target > requested_) {
            if (!overSince_) overSince_ = now;
            if (now - overSince_ < ClockSecond / 2) return requested_;
        } else overSince_ = 0;
        if (adjusted_ && now - adjusted_ < ClockSecond / 10) return requested_;
        adjusted_ = now;
        if (target > requested_) requested_ += std::min(target - requested_, uint32_t(1000 * 60));
        else requested_ -= std::min(requested_ - target, uint32_t(250 * 60));
        return requested_;
    }
    uint32_t Requested() const { return requested_; }
  private:
    uint32_t requested_ = Normal;
    int64_t adjusted_ = 0, overSince_ = 0;
};
}
