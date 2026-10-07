#include "PreciseWait.hpp"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#include <chrono>
#include <thread>
#endif

namespace cccaster::platform {
int64_t RealMonotonicTicks() {
#ifdef _WIN32
    // 32bitでも初期化中の64bit値を他スレッドに見せない。
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return int64_t(value.QuadPart);
    }();
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    if (!frequency) return int64_t(GetTickCount64()) * 60000;
    const auto q = int64_t(value.QuadPart);
    if (frequency == 10000000) return q * 6;
    // q全体を乗算せず、長時間稼働時のオーバーフローを避ける。
    return (q / frequency) * 60000000LL + (q % frequency) * 60000000LL / frequency;
#else
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return int64_t(value.tv_sec) * 60000000LL + value.tv_nsec * 60LL / 1000;
#endif
}
int64_t RealMonotonicUs() { return RealMonotonicTicks() / 60; }
void CpuRelax() {
#ifdef _WIN32
    YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
namespace {
#ifdef _WIN32
struct WaitTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    WaitTimer() { if (!handle) handle = CreateWaitableTimerW(nullptr, FALSE, nullptr); }
    ~WaitTimer() { if (handle) CloseHandle(handle); }
};
#endif
struct WaitBackend {
    int64_t Now() const { return RealMonotonicTicks(); }
    void Relax() const { CpuRelax(); }
    void SleepUntil(int64_t target) const {
#ifdef _WIN32
        // スレッド間でハンドルを共用しない。初期化時間も絶対締切から差し引く。
        thread_local WaitTimer timer;
        auto remaining = target - Now();
        if (remaining <= 0) return;
        // 極端に長い指定でもWin32のtimeout／単位換算を溢れさせない。
        const auto chunk = std::min<int64_t>(remaining, 60000000);
        LARGE_INTEGER due{};
        due.QuadPart = -((chunk + 5) / 6); // 100nsへ切上げ。期限前復帰は外側でも再確認。
        if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            if (WaitForSingleObject(timer.handle, DWORD((chunk + 59999) / 60000 + 10)) == WAIT_OBJECT_0)
                return;
            CancelWaitableTimer(timer.handle);
        }
        // 作成・設定・待機が失敗しても、長い全区間スピンへ化けさせない。
        remaining = target - Now();
        if (remaining > 0)
            ::Sleep(DWORD((std::min<int64_t>(remaining, 60000000) + 59999) / 60000));
#else
        const auto remaining = target - Now();
        if (remaining > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(
                (std::min<int64_t>(remaining, 60000000) * 1000 + 59) / 60));
#endif
    }
};
}
int64_t PreciseWaitUntilTicks(int64_t deadlineTicks) {
    WaitBackend backend;
    return precise_wait_detail::WaitUntil(backend, deadlineTicks, PreciseSpinUs * 60);
}
void PreciseWaitUs(int64_t durationUs) {
    if (durationUs > 0)
        PreciseWaitUntilTicks(precise_wait_detail::DeadlineAfterUs(RealMonotonicTicks(), durationUs));
}
void RealSleepUs(int64_t durationUs) {
    if (durationUs <= 0) return;
    WaitBackend backend;
    precise_wait_detail::WaitUntil(backend,
        precise_wait_detail::DeadlineAfterUs(RealMonotonicTicks(), durationUs), 0);
}
}
