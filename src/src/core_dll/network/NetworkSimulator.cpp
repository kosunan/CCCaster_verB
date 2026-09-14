#include "core_dll/network/NetworkSimulator.hpp"
#include <algorithm>
#include <chrono>

namespace cccaster::network {

NetworkSimulator &NetworkSimulator::Instance() {
    static NetworkSimulator instance;
    return instance;
}

void NetworkSimulator::Enable(uint32_t minDelayMs, uint32_t maxDelayMs, uint32_t lossPercent, bool receiveOnly) {
    std::lock_guard<std::mutex> lock(_mtx);
    _minDelayMs = minDelayMs;
    _maxDelayMs = std::max(minDelayMs, maxDelayMs); // min <= max を保証
    _lossPercent = std::min(lossPercent, uint32_t(100));
    _receiveOnly.store(receiveOnly);
    _enabled.store(true, std::memory_order_release);
}

bool NetworkSimulator::IsEnabled() const {
    return _enabled.load(std::memory_order_acquire);
}

uint32_t NetworkSimulator::GetRandomDelayMs() {
    std::lock_guard<std::mutex> lock(_mtx);
    if (_minDelayMs == _maxDelayMs)
        return _minDelayMs;
    std::uniform_int_distribution<uint32_t> dist(_minDelayMs, _maxDelayMs);
    return dist(_rng);
}

bool NetworkSimulator::ShouldDrop() {
    if (_lossPercent == 0)
        return false;
    std::lock_guard<std::mutex> lock(_mtx);
    std::uniform_int_distribution<uint32_t> dist(0, 99);
    const bool dropped = dist(_rng) < _lossPercent;
    if (dropped)
        _dropped.fetch_add(1);
    return dropped;
}

int64_t NetworkSimulator::NowUs() const {
    if (auto clock = _clock.load())
        return clock();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void NetworkSimulator::ObserveDelay(uint32_t requestedMs, int64_t observedUs) {
    _delayed.fetch_add(1);
    _requestedUs.fetch_add(uint64_t(requestedMs) * 1000);
    _observedUs.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, observedUs)));
}

} // namespace cccaster::network
