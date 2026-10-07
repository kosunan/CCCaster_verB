#pragma once
#include "core_dll/timing/BoundaryCpuPlan.hpp"
#include "core_dll/timing/SpinAssist.hpp"
#include "core_dll/common/DeadlineExecutor.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <array>
#include <cstdlib>
#include <algorithm>

namespace cccaster::core::timer {
// 共通実行器の最大4担当で、境界採時から結果公開まで実行する。
class BoundaryRace {
  public:
    using Work = platform::DeadlineWork;
    static int Requested() {
        static const int count = BoundaryWorkerLimit(std::getenv("CCCASTER_BOUNDARY_WORKERS"));
        return count;
    }
    struct Result {
        int64_t boundary = 0, game = 0, armed = 0, due = 0;
        int workers = 0, valid = 0, ready = 0, winner = -1;
        std::array<int64_t, 4> stamps{};
        int64_t completed = 0, publicationUpper = 0;
        bool upperFromReader = false;
    };
    BoundaryRace() {
        const auto excluded = platform::CurrentPhysicalCoreMask();
        auto candidates = platform::BoundaryCpuCandidates(excluded);
        int capacity = 0;
        for (auto mask = candidates; mask; mask &= mask - 1) ++capacity;
        const int limit = std::min(Requested(), capacity);
        std::array<int, 4> cpus{-1, -1, -1, -1};
        if (const char *text = std::getenv("CCCASTER_BOUNDARY_CPUS"); text && *text) {
            for (int i = 0; i < Requested(); ++i) {
                char *end = nullptr;
                const auto cpu = std::strtol(text, &end, 10);
                if (end == text || cpu < 0 || cpu >= 32 || (*end && *end != ',')) return;
                cpus[i] = int(cpu);
                if (i + 1 < Requested()) { if (*end != ',') return; text = end + 1; }
                else if (*end) return;
            }
        }
        platform::DeadlineExecutor::Options options;
        options.workers = limit;
        options.enter = [this, cpus, excluded](int index) {
            pins_[index] = std::make_unique<platform::TimingCpuPin>(cpus[index], excluded);
            const auto cpu = pins_[index]->Cpu();
            domain::session::DebugLog("[BoundaryWorker] index=%d cpu=%d active=%d", index, cpu, int(cpu >= 0));
            if (cpu < 0) return false;
            priorities_[index] = std::make_unique<platform::TimingThread>("boundary");
            return true;
        };
        options.leave = [this](int index) { priorities_[index].reset(); pins_[index].reset(); };
        if (SpinPrototype::Any())
            options.auxiliary = [this](int index, int64_t now) { return assist_.Tick(index, now); };
        executor_ = std::make_unique<platform::DeadlineExecutor>(std::move(options));
        domain::session::DebugLog("[BoundaryPool] requested=%d capacity=%d active=%d game=%x candidates=%x",
            Requested(), capacity, WorkerCount(), excluded, candidates);
    }
    bool Enabled() const { return WorkerCount() != 0; }
    bool HasWork() const { return work_ != nullptr; }
    int WorkerCount() const { return executor_ ? executor_->WorkerCount() : 0; }
    void Assist(SpinChannels::Channel channel, SpinTask &task) {
        assist_.Publish(channel, task);
        if (executor_) executor_->Notify();
    }
    void Retire(SpinChannels::Channel channel) { assist_.Retire(channel); }
    SpinTask *Observe(SpinChannels::Channel channel, int64_t start, int64_t end, SpinTask::Probe probe, int64_t argument) {
        auto task = assist_.Observe(channel, start, end, probe, argument);
        if (executor_) executor_->Notify();
        return task;
    }
    void Detach(SpinChannels::Channel channel) { assist_.Detach(channel); }
    void Arm(int64_t due, int64_t now) {
        due_ = due; armed_ = now;
        const bool stall = BoundaryStallEnabled(std::getenv("CCCASTER_TEST_BOUNDARY_STALL")) && ++iteration_ % 60 == 0;
        // 不変の値を依頼自身に保持。遅い旧担当を待たず次の依頼へ移れる。
        try {
            work_ = std::make_shared<Work>(due, [due, stall](int worker, int64_t started) {
                if (stall && worker == 0) {
                    platform::RealSleepMs(3); // 処理中の停止。採用担当の成績へ混入させない。
                    domain::session::DebugLog("[BoundaryWorkStall] due=%lld started=%lld resumed=%lld",
                        due, started, platform::RealMonotonicTicks());
                }
                return uint64_t(started);
            });
        } catch (const std::bad_alloc &) {
            // 低メモリ時はこの締切だけ従来待機へ戻す。旧依頼の結果を流用しない。
            // この経路でログ整形などの追加メモリ確保は行わない。
            work_.reset();
        }
        if (executor_) executor_->Submit(work_);
    }
    Result Read(int64_t game) const {
        Result result;
        result.boundary = result.game = game; result.armed = armed_; result.due = due_;
        result.workers = WorkerCount();
        if (!work_ || !Enabled()) return result;
        // 0担当への縮退、または全担当がまだ完了していない場合は呼出側も同じ処理に参加。
        if (!work_->Ready()) work_->Execute(Work::Caller);
        Work::Result adopted;
        if (!work_->Read(adopted)) return result;
        result.boundary = int64_t(adopted.value);
        result.game = adopted.observed;
        result.completed = adopted.completed;
        result.publicationUpper = adopted.publicationUpper;
        result.upperFromReader = adopted.upperFromReader;
        result.winner = adopted.worker == Work::Caller ? -1 : adopted.worker;
        // 旧ログ形式にも非採用担当の遅れは載せない。readyは採用担当の締切前監視。
        if (result.winner >= 0) {
            result.valid = 1; result.ready = int(adopted.watching && adopted.watching < due_);
            result.stamps[result.winner] = result.boundary;
        }
        return result;
    }
  private:
    SpinChannels assist_;
    std::array<std::unique_ptr<platform::TimingCpuPin>, 4> pins_;
    std::array<std::unique_ptr<platform::TimingThread>, 4> priorities_;
    // 逆順破棄でexecutorを先にjoinし、コールバック先と依頼を最後まで保持する。
    std::shared_ptr<Work> work_;
    std::unique_ptr<platform::DeadlineExecutor> executor_;
    int64_t due_ = 0, armed_ = 0;
    unsigned iteration_ = 0;
};
}
