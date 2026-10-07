#pragma once
#include "core_dll/timing/BoundarySlot.hpp"
#include "core_dll/common/Platform.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>

namespace cccaster::core::timer {
// 試作は個別にopt-in。空の環境変数を有効と解釈しない。
struct SpinPrototype {
    static bool Flag(const char *name) { const auto p = std::getenv(name); return p && !std::strcmp(p, "1"); }
    static bool Publication() { static const bool on = Flag("CCCASTER_SPIN_PUBLICATION"); return on; }
    static bool Capture() { static const bool on = Flag("CCCASTER_SPIN_CAPTURE"); return on; }
    static bool Any() { return Publication() || Capture(); }
};

// 引数は公開からRetire完了まで不変。コールバックはゲーム状態を操作しない。
// start/endは起床窓だけを決め、成立判定は元の時計/入力公開条件で行う。
struct SpinTask {
    using Probe = bool (*)(void *, int);
    int64_t start, end;
    Probe probe;
    void *context;
    std::array<BoundarySlot, 4> observations;
    SpinTask(int64_t begin, int64_t finish, Probe check, void *arg)
        : start(begin), end(finish), probe(check), context(arg) {}
    void Reset(int64_t begin, int64_t finish, Probe check) {
        start = begin; end = finish; probe = check;
        for (auto &slot : observations) slot.Publish({0, 0, 0});
    }
    int64_t Observed(int64_t actual, int *winner = nullptr) const {
        int64_t earliest = actual;
        if (winner) *winner = -1;
        for (int i = 0; i < 4; ++i) {
            BoundarySlot::Sample sample;
            if (observations[i].Read(1, sample) && sample.stamp <= earliest) {
                earliest = sample.stamp;
                if (winner) *winner = i;
            }
        }
        return earliest;
    }
    bool Ready() const {
        for (const auto &slot : observations) {
            BoundarySlot::Sample sample;
            if (slot.Read(1, sample)) return true;
        }
        return false;
    }
};

// 入力公開・採取が既存の最大4時計を共用する。各チャネルの所有者は1スレッド。
// seq_cstのhazard公開→再確認により、Retire後はスタック上の引数も破棄できる。
class SpinChannels {
  public:
    enum Channel { Publication, Capture, Count };
    void Publish(Channel channel, SpinTask &task) { tasks_[channel].store(&task); }
    void Detach(Channel channel) { tasks_[channel].store(nullptr); }
    // ゲーム側の観測には永続スロットを使う。最大4読手に対し5枠あれば、
    // 中断された読手の終了を待たずに別枠を公開できる。破棄は全worker join後。
    SpinTask *Observe(Channel channel, int64_t begin, int64_t finish, SpinTask::Probe probe, int64_t argument) {
        Detach(channel);
        for (auto &slot : storage_[channel]) {
            bool referenced = false;
            for (auto &worker : hazards_) if (worker[channel].load() == &slot.task) referenced = true;
            if (referenced) continue;
            slot.argument = argument;
            slot.task.Reset(begin, finish, probe);
            Publish(channel, slot.task);
            return &slot.task;
        }
        return nullptr; // 将来読手数を変更しても、元の待機へ戻せる。
    }
    void Retire(Channel channel) {
        auto task = tasks_[channel].exchange(nullptr);
        if (!task) return;
        for (auto &worker : hazards_)
            while (worker[channel].load() == task) std::this_thread::yield();
    }
    int64_t Tick(int worker, int64_t now) {
        int64_t next = now + 100000 * 60;
        for (int c = 0; c < Count; ++c) {
            auto task = tasks_[c].load();
            if (!task) continue;
            hazards_[worker][c].store(task);
            struct Clear {
                std::atomic<SpinTask *> &hazard;
                ~Clear() { hazard.store(nullptr); }
            } clear{hazards_[worker][c]};
            if (tasks_[c].load() == task) {
                BoundarySlot::Sample previous;
                if (now <= task->end && !task->observations[worker].Read(1, previous)) {
                    if (now < task->start) next = std::min(next, task->start);
                    else if (task->probe(task->context, worker))
                        task->observations[worker].Publish({1, platform::RealMonotonicTicks(), now});
                    else next = now;
                }
            }
        }
        return next;
    }
  private:
    struct Observation {
        int64_t argument = 0;
        SpinTask task{0, 0, nullptr, &argument};
    };
    std::array<std::array<Observation, 5>, Count> storage_{};
    std::array<std::atomic<SpinTask *>, Count> tasks_{};
    std::array<std::array<std::atomic<SpinTask *>, Count>, 4> hazards_{};
};
}
