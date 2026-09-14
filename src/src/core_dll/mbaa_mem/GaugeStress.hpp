#pragma once
// 対象版のDLL初期版照合を通った実機・自動入力試験だけで使用する。
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/DeferredNumericLog.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/timing/SpeedFlags.hpp"

namespace cccaster::testing::gauge_stress {
inline bool active = false, pending = false;
inline uint32_t frame = 0, requested = 0;
inline int64_t started = 0, previousNormal = 0, interval = 0;
inline unsigned Mode() {
    static const unsigned mode = [] {
        const char *v = std::getenv("CCCASTER_GAUGE_STRESS");
        return IsScriptedInputEnabled() && v && (v[0]=='1' || v[0]=='2') && v[1]=='\0' ? unsigned(v[0]-'0') : 0u;
    }();
    return mode;
}
inline void Freeze() {
    *CC_ROUND_TIMER_ADDR = 4752;
    *CC_P1_HEALTH_ADDR = *CC_P1_RED_HEALTH_ADDR = 11400;
    *CC_P2_HEALTH_ADDR = *CC_P2_RED_HEALTH_ADDR = 11400;
}
inline void Begin(bool combat, uint32_t f) {
    const bool wasActive = active;
    active = Mode() && combat;
    if (!active) { pending = false; previousNormal = 0; return; }
    if (!wasActive) diagnostics::DeferredNumericLog::Log("[GaugeStressConfig] mode=%u period=60 firstRise=660 health=11400 timer=4752",Mode());
    pending = true;
    frame = f;
    // 2は固定0%の対照。双方・通常進行・再計算ともフレーム番号だけで決める。
    requested = Mode()==1 && f%65536 >= 600 && (((f%65536)-600)/60)%2 ? 30000 : 0;
    Freeze();
    *CC_P1_METER_ADDR = *CC_P2_METER_ADDR = requested;
    started = platform::RealMonotonicTicks();
}
inline void Flush() {
    if (!pending) return;
    const auto elapsed = platform::RealMonotonicTicks()-started;
    const bool replay = core::SpeedFlags::RenderSkip().load();
    interval = !replay && previousNormal ? started-previousNormal : 0;
    if (!replay) previousNormal = started;
    pending = false;
    const auto hp1=*CC_P1_HEALTH_ADDR, hp2=*CC_P2_HEALTH_ADDR, timer=*CC_ROUND_TIMER_ADDR;
    // 表示時にも同じ体力・時間を保つ。WT・RT・独立入力時計には触れない。
    Freeze();
    domain::session::DebugLog("[GaugeStress] f=%u requested=%u meter1=%u meter2=%u heat1=%u heat2=%u hp1=%u hp2=%u timer=%u ticks=%lld interval=%lld replay=%d",
        frame, requested, *CC_P1_METER_ADDR, *CC_P2_METER_ADDR, *CC_P1_HEAT_ADDR, *CC_P2_HEAT_ADDR,
        hp1,hp2,timer,elapsed,interval,core::SpeedFlags::RenderSkip().load());
}
}
