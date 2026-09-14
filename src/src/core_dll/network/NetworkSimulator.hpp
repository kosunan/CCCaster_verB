#pragma once
// ============================================================================
// NetworkSimulator.hpp — ネットワーク遅延・パケットロスのシミュレーション
//
// テスト用途で送受信パケットにランダム遅延とランダムドロップを挿入する。
// グローバルシングルトンとして動作し、CLI引数 (--sim-delay, --sim-loss) で有効化。
// ============================================================================
#include <cstdint>
#include <random>
#include <atomic>
#include <mutex>

namespace cccaster::network {

class NetworkSimulator {
  public:
    static NetworkSimulator &Instance();

    /// シミュレーションを有効化する
    /// @param minDelayMs 最小遅延 (ms)
    /// @param maxDelayMs 最大遅延 (ms)
    /// @param lossPercent パケットロス率 (0-100)
    void Enable(uint32_t minDelayMs, uint32_t maxDelayMs, uint32_t lossPercent, bool receiveOnly = false);

    /// シミュレーションが有効かどうか
    bool IsEnabled() const;
    bool IsSendEnabled() const { return IsEnabled() && !_receiveOnly.load(); }

    /// ランダムな遅延値を取得 (ms)
    uint32_t GetRandomDelayMs();

    /// パケットをドロップすべきか判定 (true = ドロップ)
    bool ShouldDrop();
    using Clock = int64_t (*)();
    void SetClock(Clock clock) {
        _clock.store(clock);
    }
    int64_t NowUs() const;
    void ObserveDelay(uint32_t requestedMs, int64_t observedUs);
    uint64_t DelayedCount() const {
        return _delayed.load();
    }
    uint64_t DroppedCount() const {
        return _dropped.load();
    }
    uint64_t RequestedUs() const {
        return _requestedUs.load();
    }
    uint64_t ObservedUs() const {
        return _observedUs.load();
    }

    // 設定値の取得 (ログ表示用)
    uint32_t GetMinDelayMs() const {
        return _minDelayMs;
    }
    uint32_t GetMaxDelayMs() const {
        return _maxDelayMs;
    }
    uint32_t GetLossPercent() const {
        return _lossPercent;
    }

  private:
    NetworkSimulator() = default;
    NetworkSimulator(const NetworkSimulator &) = delete;
    NetworkSimulator &operator=(const NetworkSimulator &) = delete;

    std::atomic<bool> _enabled{false};
    std::atomic<bool> _receiveOnly{false};
    std::atomic<Clock> _clock{nullptr};
    std::atomic<uint64_t> _delayed{0}, _dropped{0}, _requestedUs{0}, _observedUs{0};
    uint32_t _minDelayMs = 0;
    uint32_t _maxDelayMs = 0;
    uint32_t _lossPercent = 0;
    std::mt19937 _rng{std::random_device{}()};
    std::mutex _mtx;
};

} // namespace cccaster::network
