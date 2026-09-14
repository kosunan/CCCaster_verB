#include "core_dll/timing/ThreadSignal.hpp"
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
void require(bool ok) {
    if (!ok)
        std::abort();
}
int main() {
    using cccaster::core::timer::ThreadSignal;
    ThreadSignal signal, done;
    auto ticket = signal.Ticket();
    signal.Notify();
    require(signal.Wait(ticket, 0)); // 待機開始より前の通知も失わない。
    require(!signal.Wait(signal.Ticket(), 1000));
    std::atomic<bool> running{true};
    std::atomic<unsigned> processed{0};
    std::thread worker([&] {
        auto observed = signal.Ticket();
        done.Notify();
        while (running.load()) {
            signal.Wait(observed, -1);
            observed = signal.Ticket();
            ++processed;
            done.Notify();
        }
    });
    while (!processed.load()) {
        auto acknowledgement = done.Ticket();
        signal.Notify();
        done.Wait(acknowledgement, 100000);
    }
    running = false;
    signal.Notify(); // 無期限待機を停止時に解除する。
    worker.join();
    std::puts("thread signal OK");
}
