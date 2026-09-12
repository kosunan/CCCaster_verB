#pragma once
#include <cstdint>

namespace cccaster::core::sync {
enum class WaitResult { Ready, Cancelled, Disconnected, Timeout };

// 時計/待機/疎通を注入して期限と中断を実時間に依存せず検証できる。
template <class Ready, class Cancel, class Alive, class Clock, class Sleep>
WaitResult WaitBounded(int64_t timeoutUs, Ready ready, Cancel cancel, Alive alive, Clock clock, Sleep sleep) {
    const int64_t started = clock();
    for (;;) {
        if (cancel())
            return WaitResult::Cancelled;
        if (!alive())
            return WaitResult::Disconnected;
        if (clock() - started >= timeoutUs)
            return WaitResult::Timeout;
        if (ready())
            return WaitResult::Ready;
        sleep();
    }
}
} // namespace cccaster::core::sync
