#pragma once
#include "PreciseWait.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace cccaster::platform {
// 締切監視→処理→公開を各担当が行い、公開に成功した1本の結果を採用する。
// Functionは複数担当から同時に実行できる処理。共有状態の変更には呼出側の
// 排他／世代検査が必要。ゲームスレッド専用処理を移すためのAPIではない。
class DeadlineWork {
  public:
    static constexpr int Helpers = 4, Caller = 4;
    using Function = std::function<uint64_t(int worker, int64_t started)>;
    struct Result {
        uint64_t value = 0;
        int worker = -1;
        int64_t due = 0, watching = 0, started = 0, completed = 0;
        // 公開CASの直後に採時した上限。担当がCAS直後に停止した場合は読手の採時。
        // completedとpublicationUpperの間に結果は公開された。非採用者は集計しない。
        int64_t publicationUpper = 0, observed = 0;
        bool upperFromReader = false;
    };
    DeadlineWork(int64_t due, Function function) : due_(due), function_(std::move(function)) {}
    int64_t Due() const { return due_; }
    bool Ready() const { return winner_.load(std::memory_order_acquire) >= 0; }
    bool Attempted(int worker) const { return slots_[worker].entered.load(std::memory_order_relaxed); }
    void Watch(int worker, int64_t now) { if (!slots_[worker].watching) slots_[worker].watching = now; }
    bool Execute(int worker);
    bool Read(Result &result) const;
  private:
    struct alignas(64) Slot {
        std::atomic<bool> entered{false};
        uint64_t value = 0;
        int64_t watching = 0, started = 0, completed = 0;
        std::atomic<int64_t> publishedAfter{0};
    };
    const int64_t due_;
    const Function function_;
    std::array<Slot, Helpers + 1> slots_;
    std::atomic<int> winner_{-1};
};

// 常設の最大4担当。Submitは単一の所有スレッドから行う。
// 依頼と引数の寿命はshared_ptrで維持し、採用されなかった遅い担当の終了を待たず
// 次の依頼へ進める。新規依頼・取消し・終了で休止中の担当を起こす。
class DeadlineExecutor {
  public:
    struct Options {
        int workers = 4;
        // CPU固定／優先度等の方針を利用側から指定する。部品自身は変更しない。
        std::function<bool(int)> enter;
        std::function<void(int)> leave;
        // 既存の入力監視を同じ担当で実行する場合の補助巡回。戻り値は次の実時刻。
        std::function<int64_t(int, int64_t)> auxiliary;
    };
    explicit DeadlineExecutor(Options options);
    ~DeadlineExecutor();
    int WorkerCount() const;
    void Submit(std::shared_ptr<DeadlineWork> work);
    void Notify();
    DeadlineExecutor(const DeadlineExecutor &) = delete;
    DeadlineExecutor &operator=(const DeadlineExecutor &) = delete;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
