#pragma once
#include "core_dll/mbaa_mem/GamePhase.hpp"
#include "core_dll/engine/LocalInputGate.hpp"
#include "core_dll/timing/FrameCadence.hpp"
#include "core_dll/timing/PhaseFollower.hpp"
#include <atomic>
#include <array>
#include "core_dll/sync/MatchInputBuffer.hpp"
#include <mutex>
namespace cccaster::core::sync {
// 専用時計スレッドがWASAPIの締切で入力を採取。ゲームスレッドは保存済み枠を消費する。
class InputTimeline {
  public:
    static InputTimeline &GetInstance();
    void Reset();
    void Begin(uint32_t base, uint32_t firstCapture, game_interface::GamePhase phase, bool host, int64_t firstTicks = 0);
    void Pause();
    void Resume();
    void PumpTicks(int64_t nowTicks, int64_t periodCorrectionParts);
    // テスト/旧呼出元だけの互換入口。実入力スレッドはPumpTicksを使用。
    void Pump(int64_t nowUs, int64_t intervalUs, int64_t nowTicks = 0) {
        PumpTicks(nowTicks ? nowTicks : nowUs*60, (intervalUs*60-999960)*timer::ClockParts);
    }
    int64_t NextDeadlineUs();
    int64_t NextDeadlineTicks();
    int64_t CapturedDeadlineTicks(uint32_t frame);
    int64_t CapturedDeadlineUs(uint32_t frame);
    void TraceCapturedPhase(uint32_t frame);
    bool IsActive() const {
        return active_.load(std::memory_order_acquire);
    }
    bool HasCaptured(uint32_t frame) const {
        return sampled_.load(std::memory_order_acquire) >= frame;
    }
    bool HasOverflowed() const {
        return overflow_.load(std::memory_order_acquire);
    }
    void SetConsumed(uint32_t frame) {
        consumed_.store(frame, std::memory_order_release);
    }
    uint32_t SampledFrame() const {
        return sampled_.load(std::memory_order_acquire);
    }

  private:
    void PublishSchedule();
    struct CaptureTime {
        uint32_t frame = 0;
        int64_t dueTicks = 0;
        int64_t error = 0, shift = 0, theta = 0, rtt = 0;
        int64_t phaseParts = 0, rateParts = 0;
        uint32_t revision = 0;
        bool ready = false;
    };
    std::array<CaptureTime, MatchInputBuffer::RING_SIZE> captureTimes_{};
    int64_t phaseParts_ = 0, rateParts_ = 0;
    uint32_t modelRevision_ = 0;
    bool modelReady_ = false;
    int64_t phaseError_ = 0, phaseShift_ = 0, phaseTheta_ = 0, phaseRtt_ = 0;
    std::mutex mutex_;
    std::atomic<bool> active_{false}, overflow_{false};
    std::atomic<uint32_t> sampled_{0}, consumed_{0};
    uint32_t base_ = 0, next_ = 0, lastValue_ = 0;
    bool host_ = false, starting_ = false;
    game_interface::GamePhase phase_{};
    timer::FrameCadence cadence_;
    timer::PhaseFollower follower_;
    domain::scene::LocalInputGate gate_;
};
} // namespace cccaster::core::sync
