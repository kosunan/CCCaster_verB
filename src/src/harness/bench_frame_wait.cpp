// 単独計測。ゲームAPIはフックせず、同じWASAPI締切に対する待機誤差を比較する。
#include "core_dll/common/Platform.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/timing/FrameCadence.hpp"
#include "core_dll/hook/TimeHooks.hpp"
#include <algorithm>
#include <vector>
#include <cstdio>
namespace cccaster::core::hooks {
void TimeHooks::RealQueryPerformanceCounter(LARGE_INTEGER *p) {
    ::QueryPerformanceCounter(p);
}
void TimeHooks::RealSleep(DWORD ms) {
    ::Sleep(ms);
}
} // namespace cccaster::core::hooks
int main() {
    using namespace cccaster;
    platform::BeginHighResolutionTimers();
    for (int mode = 0; mode < 2; ++mode) {
        core::timer::FrameCadence cadence;
        cadence.Reset(core::timer::WasapiClock::GetTimeUs());
        std::vector<int64_t> late;
        for (int i = 0; i < 120; ++i) {
            cadence.Advance(16666);
            int64_t now = 0;
            while ((now = core::timer::WasapiClock::GetTimeUs()) < cadence.NextUs()) {
                if (mode)
                    platform::PreciseWaitUs(std::min<int64_t>(1000, cadence.NextUs() - now));
                else
                    platform::RealSleepMs(1);
            }
            late.push_back(now - cadence.NextUs());
        }
        std::sort(late.begin(), late.end());
        std::printf("%s WASAPI=%d n=120 late_us median=%lld p95=%lld max=%lld\n", mode ? "precise" : "sleep1",
                    core::timer::WasapiClock::GetInstance().IsAvailable(), late[60], late[114], late.back());
    }
    platform::EndHighResolutionTimers();
}
