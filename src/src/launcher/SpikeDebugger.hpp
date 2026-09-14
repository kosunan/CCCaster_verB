#pragma once
#include <atomic>
#include <thread>
#include <windows.h>
namespace cccaster::main_app {
// 自分が起動した子だけを診断。通常時はスレッドもファイルも作らない。
class SpikeDebugger {
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<int> ready_{0};
    void Run(DWORD pid, DWORD tid);
  public:
    bool Start(DWORD pid, DWORD tid);
    void Stop();
    ~SpikeDebugger() { Stop(); }
};
}
