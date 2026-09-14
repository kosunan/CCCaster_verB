#include "core_dll/timing/Metronome.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include <cstdlib>
#include <cstdio>
static int64_t now = 60000000;
static unsigned preciseCalls = 0, sleepCalls = 0;
void HookLog(const char *) {}
namespace cccaster::core::timer {
int64_t WasapiClock::GetTimeTicks() { return now; }
}
namespace cccaster::platform {
int64_t RealMonotonicTicks() { return now; }
void RealSleepMs(uint32_t ms) { now += ms * 60000; ++sleepCalls; }
void PreciseWaitUs(int64_t us) { now += us * 60; ++preciseCalls; }
void CpuRelax() { now += 6; }
}
static void require(bool condition) { if (!condition) std::abort(); }
int main() {
#ifdef _WIN32
    _putenv("CCCASTER_TIME_SCALE=1");
    _putenv("CCCASTER_TEST_OFFLINE_PACING=");
#else
    setenv("CCCASTER_TIME_SCALE", "1", 1);
    unsetenv("CCCASTER_TEST_OFFLINE_PACING");
#endif
    cccaster::core::netplay::Metronome clock;
    clock.Start();
    const auto first = clock.WaitForNextTick(false, 200 * 60);
    require(first == 61000000 && now >= first - 12000 && now < first - 12000 + 6);
    require(preciseCalls > 0 && sleepCalls == 0);
    now += 83 * 60; // 入力準備83µsを周期へ加えない。
    const auto second = clock.WaitForNextTick(false, 200 * 60);
    require(second - first == 1000000);
    require(now >= second - 12000 && now < second - 12000 + 6);
    now = second + 1100000; // 次の締切を少し超える処理も、周期の基準を動かさない。
    const auto late = now;
    require(clock.WaitForNextTick(false, 12000) == second + 1000000 && now == late);
    const auto skipped = clock.WaitForNextTick(true);
    require(skipped == second + 2000000 && now == late);
    const auto normal = clock.WaitForNextTick(false);
    require(normal == second + 3000000 && now >= normal && now < normal + 6);
    require(sleepCalls > 0); // 既定の補助待機の経路は維持。
    clock.Stop();
    require(!clock.IsRunning());
    std::puts("metronome: 絶対締切・準備余裕・遅延・待機省略 OK");
}
