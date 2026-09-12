#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cccaster::core::timer {
// 単一消費者向け。待機直前の通知も世代番号で保持し、通知の取りこぼしを防ぐ。
// 通知はまとめて扱う。フレーム本体のキューは既存の中央バッファを使用する。
class ThreadSignal {
  public:
#ifdef _WIN32
    ThreadSignal() {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
        highResolution_ = timer_ != nullptr;
        if (!timer_)
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~ThreadSignal() {
        if (timer_) {
            CancelWaitableTimer(timer_);
            CloseHandle(timer_);
        }
        if (event_)
            CloseHandle(event_);
    }
#endif
    bool IsHighResolution() const {
#ifdef _WIN32
        return highResolution_;
#else
        return false;
#endif
    }
    uint64_t Ticket() {
        std::lock_guard lock(mutex_);
        return generation_;
    }
    void Notify() {
        {
            std::lock_guard lock(mutex_);
            ++generation_;
        }
#ifdef _WIN32
        if (event_)
            SetEvent(event_);
#endif
        changed_.notify_one();
    }
    bool Wait(uint64_t ticket, int64_t timeoutUs) {
#ifdef _WIN32
        if (event_ && timer_) {
            if (Ticket() != ticket)
                return true;
            if (timeoutUs == 0)
                return false;
            bool armed = timeoutUs < 0;
            if (timeoutUs > 0) {
                LARGE_INTEGER due{};
                due.QuadPart = -timeoutUs * 10;
                armed = SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
            }
            if (armed) {
                HANDLE handles[] = {event_, timer_};
                for (;;) {
                    if (Ticket() != ticket) {
                        if (timeoutUs > 0)
                            CancelWaitableTimer(timer_);
                        return true;
                    }
                    const auto result =
                        WaitForMultipleObjects(timeoutUs < 0 ? 1 : 2, handles, FALSE, INFINITE);
                    if (result == WAIT_OBJECT_0)
                        continue; // 古い通知なら残りの締切まで待つ。
                    if (timeoutUs > 0)
                        CancelWaitableTimer(timer_);
                    if (result == WAIT_OBJECT_0 + 1)
                        return Ticket() != ticket;
                    break;
                }
            }
        }
#endif
        std::unique_lock lock(mutex_);
        const auto changed = [&] { return generation_ != ticket; };
        if (timeoutUs < 0) {
            changed_.wait(lock, changed);
            return true;
        }
        return changed_.wait_for(lock, std::chrono::microseconds(timeoutUs), changed);
    }

  private:
#ifdef _WIN32
    HANDLE event_ = nullptr, timer_ = nullptr;
    bool highResolution_ = false;
#endif
    std::mutex mutex_;
    std::condition_variable changed_;
    uint64_t generation_ = 0;
};
} // namespace cccaster::core::timer
