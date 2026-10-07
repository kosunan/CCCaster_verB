#pragma once
#include "core_dll/timing/BoundaryRace.hpp"
#include "core_dll/timing/WasapiClock.hpp"
namespace cccaster::core::timer {
class SpinObservationScope {
  public:
    SpinObservationScope(bool enabled, SpinChannels::Channel channel, int64_t start, int64_t end,
                         SpinTask::Probe probe, int64_t argument)
        : pool_(enabled ? WasapiClock::SharedHelpers() : nullptr), channel_(channel),
          task_(pool_ ? pool_->Observe(channel, start, end, probe, argument) : nullptr) {}
    ~SpinObservationScope() { Finish(); }
    void Finish() { if (pool_) { pool_->Detach(channel_); pool_ = nullptr; } }
    bool Ready() const { return task_ && task_->Ready(); }
    int Workers() const { return pool_ ? pool_->WorkerCount() : 0; }
    int64_t Observed(int64_t actual, int *winner) const {
        *winner = -1;
        return task_ ? task_->Observed(actual, winner) : actual;
    }
    SpinObservationScope(const SpinObservationScope &) = delete;
    SpinObservationScope &operator=(const SpinObservationScope &) = delete;
  private:
    BoundaryRace *pool_;
    SpinChannels::Channel channel_;
    SpinTask *task_;
};
class SpinAssistScope {
  public:
    SpinAssistScope(bool enabled, SpinChannels::Channel channel, SpinTask &task)
        : pool_(enabled ? WasapiClock::SharedHelpers() : nullptr), channel_(channel) {
        if (pool_) pool_->Assist(channel_, task);
    }
    ~SpinAssistScope() { Finish(); }
    void Finish() { if (pool_) { pool_->Retire(channel_); pool_ = nullptr; } }
    int Workers() const { return pool_ ? pool_->WorkerCount() : 0; }
    SpinAssistScope(const SpinAssistScope &) = delete;
    SpinAssistScope &operator=(const SpinAssistScope &) = delete;
  private:
    BoundaryRace *pool_;
    SpinChannels::Channel channel_;
};
}
