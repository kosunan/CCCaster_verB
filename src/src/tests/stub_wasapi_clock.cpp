// ============================================================================
// stub_wasapi_clock.cpp — WasapiClock の置換スタブ
//
// NetplayClock.cpp が要求する唯一の外部シンボルが WasapiClock::GetTimeUs()。
// 本物は WASAPI (COM) を初期化するため、テストでは常に 0 を返す実装に差し替える。
// NetplayClock の計算は全て引数で渡された時刻で行われるため影響しない。
// ============================================================================

#include "core_dll/timing/WasapiClock.hpp"

namespace cccaster {
namespace core {
namespace timer {

int64_t testClockTicks = 0;
uint32_t testClockSource = 0;
uint32_t WasapiClock::GetSourceGeneration() { return testClockSource; }
int64_t WasapiClock::GetTimeTicks() {
    return testClockTicks;
}
int64_t WasapiClock::GetTimeTicks(ReadSample *sample) {
    const auto time = GetTimeTicks();
    if (sample) *sample = {time, time, 0, 0, 0};
    return time;
}
int64_t WasapiClock::GetTimeUs() {
    return testClockTicks / 60;
}

} // namespace timer
} // namespace core
} // namespace cccaster
