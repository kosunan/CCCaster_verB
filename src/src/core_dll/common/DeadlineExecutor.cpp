#include "DeadlineExecutor.hpp"
#include "core_dll/timing/ThreadSignal.hpp"
#include <algorithm>
#include <future>
#include <thread>

namespace cccaster::platform {
bool DeadlineWork::Execute(int worker) {
    if (worker < 0 || worker > Caller || Ready()) return false;
    const auto now = RealMonotonicTicks();
    if (now < due_) return false;
    auto &slot = slots_[worker];
    if (slot.entered.exchange(true)) return false;
    slot.started = now;
    // 先に担当を独占しない。処理途中で止まっても他の担当が完了できる。
    slot.value = function_(worker, now);
    slot.completed = RealMonotonicTicks();
    int expected = -1;
    if (!winner_.compare_exchange_strong(expected, worker, std::memory_order_release,
                                         std::memory_order_relaxed)) return false;
    // CASが採用と結果公開を兼ねる。以降の停止でも結果自体は読み出せる。
    slot.publishedAfter.store(RealMonotonicTicks(), std::memory_order_release);
    return true;
}
bool DeadlineWork::Read(Result &result) const {
    const int winner = winner_.load(std::memory_order_acquire);
    if (winner < 0) return false;
    const auto &slot = slots_[winner];
    result.value = slot.value; result.worker = winner; result.due = due_;
    result.watching = slot.watching;
    result.started = slot.started; result.completed = slot.completed;
    result.publicationUpper = slot.publishedAfter.load(std::memory_order_acquire);
    result.observed = RealMonotonicTicks();
    result.upperFromReader = result.publicationUpper == 0;
    if (result.upperFromReader) result.publicationUpper = result.observed;
    return true;
}
struct DeadlineExecutor::Impl {
    struct Worker {
        core::timer::ThreadSignal wake;
        std::promise<bool> started;
        std::thread thread;
    };
    Options options;
    std::array<Worker, DeadlineWork::Helpers> workers;
    std::atomic<std::shared_ptr<DeadlineWork>> command;
    std::atomic<uint64_t> revision{0};
    std::atomic<bool> stop{false};
    int active = 0, launched = 0;
    explicit Impl(Options value) : options(std::move(value)) {
        std::array<std::future<bool>, DeadlineWork::Helpers> ready;
        for (int i = 0; i < std::clamp(options.workers, 0, DeadlineWork::Helpers); ++i) {
            ready[i] = workers[i].started.get_future();
            try { workers[i].thread = std::thread([this, i] { Run(i); }); ++launched; }
            catch (...) { break; }
        }
        for (int i = 0; i < launched; ++i) if (ready[i].get()) ++active;
    }
    ~Impl() {
        stop.store(true);
        Notify();
        for (auto &worker : workers) if (worker.thread.joinable()) worker.thread.join();
    }
    void Notify() { for (int i = 0; i < launched; ++i) workers[i].wake.Notify(); }
    void Run(int index) {
        bool reported = false;
        try {
            const bool enabled = !options.enter || options.enter(index);
            workers[index].started.set_value(enabled); reported = true;
            if (enabled) Loop(index);
        } catch (...) { if (!reported) workers[index].started.set_value(false); }
        if (options.leave) options.leave(index);
    }
    void Loop(int index) {
        auto &wake = workers[index].wake;
        uint64_t loaded = ~uint64_t{0};
        std::shared_ptr<DeadlineWork> work;
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) return;
            // Ticketは休止に入る外周だけ。最後の1msはmutexも共有ptrも触らない。
            const auto ticket = wake.Ticket();
            const auto current = revision.load(std::memory_order_acquire);
            if (loaded != current) { work = command.load(); loaded = current; }
            auto now = RealMonotonicTicks();
            auto next = now + 100000 * 60;
            const auto activeWork = work && !work->Ready() && !work->Attempted(index);
            if (activeWork) next = work->Due() - PreciseSpinUs * 60;
            if (options.auxiliary) next = std::min(next, options.auxiliary(index, now));
            if (next > now) { wake.Wait(ticket, (next - now + 59) / 60); continue; }
            if (activeWork && now >= work->Due() - PreciseSpinUs * 60) work->Watch(index, now);
            // 依頼更新／取消しはrevisionだけで検出。補助仕事も同じ4担当を共有する。
            do {
                if (stop.load(std::memory_order_relaxed) || revision.load(std::memory_order_acquire) != loaded)
                    break;
                now = RealMonotonicTicks();
                if (work && !work->Ready() && !work->Attempted(index) && now >= work->Due()) {
                    try { work->Execute(index); } catch (...) { /* 他の担当・呼出側は継続できる。 */ }
                }
                next = work && !work->Ready() && !work->Attempted(index)
                    ? work->Due() - PreciseSpinUs * 60 : now + 100000 * 60;
                if (options.auxiliary) next = std::min(next, options.auxiliary(index, now));
                if (next > now) break;
                CpuRelax();
            } while (true);
        }
    }
};
DeadlineExecutor::DeadlineExecutor(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
DeadlineExecutor::~DeadlineExecutor() = default;
int DeadlineExecutor::WorkerCount() const { return impl_->active; }
void DeadlineExecutor::Submit(std::shared_ptr<DeadlineWork> work) {
    impl_->command.store(std::move(work));
    impl_->revision.fetch_add(1, std::memory_order_release);
    impl_->Notify();
}
void DeadlineExecutor::Notify() { impl_->Notify(); }
}
