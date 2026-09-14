#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace cccaster::domain::ui {

struct RollbackMetricsSnapshot {
    uint32_t maxDepth = 0, count = 0;
    bool available = false;
};

// 実時間の直近1000ms。再計算開始時の1回だけ記録する。
// 固定バッファなのでゲームスレッド上で動的確保しない。
class RollbackMetricsWindow {
    struct Bucket { uint64_t ms = 0; uint32_t depth = 0, count = 0; };
    std::array<Bucket, 1000> buckets_{};
    bool available_ = false;
public:
    void Reset(bool available) { buckets_ = {}; available_ = available; }
    void Record(uint64_t nowMs, uint32_t depth) {
        if (!available_ || !depth) return;
        auto &b = buckets_[nowMs % buckets_.size()];
        if (b.ms != nowMs) b = {nowMs, 0, 0};
        b.depth = std::max(b.depth, depth);
        ++b.count;
    }
    RollbackMetricsSnapshot Read(uint64_t nowMs) const {
        RollbackMetricsSnapshot result{};
        result.available = available_;
        if (!available_) return result;
        for (const auto &b : buckets_) {
            if (b.count && nowMs >= b.ms && nowMs - b.ms < 1000) {
                result.maxDepth = std::max(result.maxDepth, b.depth);
                result.count += b.count;
            }
        }
        return result;
    }
};

struct NetworkMetricsSnapshot {
    float latestRttMs = 0, maxRttMs = 0, jitterMs = 0;
    uint32_t sampleCount = 0;
    bool available = false, stale = false, jitterAvailable = false;
};

enum class LatencyWarningSeverity : uint8_t { None, Light, Heavy };

struct LatencyWarningSnapshot {
    LatencyWarningSeverity severity = LatencyWarningSeverity::None;
    float spikeRttMs = 0.0f;
    float coverageMs = 0.0f;
    uint32_t extraFrames = 0;
    bool evaluated = false;
};

// 一瞬のRTTスパイクをHUDの1秒窓より長く保持する。
// D/Rを増やして同じスパイクを覆えた場合は、保持時間内でも警告を即時解除する。
class LatencyWarningTracker {
  public:
    static constexpr uint64_t HOLD_MS = 8000;
    static constexpr float FRAME_MS = 1000.0f / 60.0f;

    void Reset() { spikeRttMs_ = 0.0f; expiresMs_ = 0; }
    void Record(uint64_t nowMs, float rttMs) {
        if (!std::isfinite(rttMs) || rttMs < 0.0f || rttMs > 1000.0f)
            return;
        if (nowMs >= expiresMs_ || rttMs >= spikeRttMs_) {
            spikeRttMs_ = rttMs;
            expiresMs_ = nowMs + HOLD_MS;
        }
    }
    LatencyWarningSnapshot Read(uint64_t nowMs, int delay, int rollback) const {
        LatencyWarningSnapshot result{};
        result.evaluated = true;
        if (nowMs >= expiresMs_ || delay < 0 || rollback < 0)
            return result;
        const int coveredFrames = delay + rollback;
        result.coverageMs = coveredFrames * FRAME_MS;
        result.spikeRttMs = spikeRttMs_;
        if (spikeRttMs_ <= result.coverageMs)
            return result;
        result.extraFrames = static_cast<uint32_t>(std::ceil((spikeRttMs_ - result.coverageMs) / FRAME_MS));
        result.severity = result.extraFrames >= 2 ? LatencyWarningSeverity::Heavy
                                                   : LatencyWarningSeverity::Light;
        return result;
    }

  private:
    float spikeRttMs_ = 0.0f;
    uint64_t expiresMs_ = 0;
};

// 受理したNTPサンプルだけを追加する。描画回数はサンプル数に含めない。
// ジッタは窓内の連続RTT差の絶対値の平均。時計補正値とは独立した診断。
class NetworkMetricsWindow {
    struct Sample { uint64_t ms; float rtt; };
    std::deque<Sample> samples_;
    Sample last_{};
    bool available_ = false;
    void Expire(uint64_t nowMs) {
        while (!samples_.empty() && nowMs >= samples_.front().ms &&
               nowMs - samples_.front().ms >= 1000) samples_.pop_front();
    }
public:
    void Reset() { samples_.clear(); last_ = {}; available_ = false; }
    void Record(uint64_t nowMs, float rttMs) {
        if (!std::isfinite(rttMs) || rttMs < 0 || rttMs > 1000) return;
        Expire(nowMs);
        last_ = {nowMs, rttMs};
        available_ = true;
        samples_.push_back(last_);
    }
    NetworkMetricsSnapshot Read(uint64_t nowMs) const {
        NetworkMetricsSnapshot result{};
        result.available = available_;
        if (!available_) return result;
        result.latestRttMs = last_.rtt;
        result.stale = nowMs < last_.ms || nowMs - last_.ms >= 1000;
        float previous = 0;
        bool first = true;
        for (const auto &sample : samples_) {
            if (nowMs < sample.ms || nowMs - sample.ms >= 1000) continue;
            ++result.sampleCount;
            result.maxRttMs = std::max(result.maxRttMs, sample.rtt);
            if (!first) result.jitterMs += std::fabs(sample.rtt - previous);
            previous = sample.rtt;
            first = false;
        }
        result.jitterAvailable = result.sampleCount >= 2;
        if (result.jitterAvailable) result.jitterMs /= result.sampleCount - 1;
        return result;
    }
};
} // namespace cccaster::domain::ui
