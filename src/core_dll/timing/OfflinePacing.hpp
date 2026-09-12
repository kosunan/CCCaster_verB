#pragma once
#include <cstdlib>
#include <cstring>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/timing/WasapiClock.hpp"

namespace cccaster::core::timer {
// 同一バイナリで原因を切り分ける比較専用設定。対戦の経路には適用しない。
struct OfflinePacing {
    enum class Variant { Normal, Legacy, GateOnly };
    static Variant Mode() {
        static const Variant value = [] {
            const auto *text = std::getenv("CCCASTER_TEST_OFFLINE_PACING");
            if (text && !std::strcmp(text, "legacy")) return Variant::Legacy;
            if (text && !std::strcmp(text, "gate-only")) return Variant::GateOnly;
            return Variant::Normal;
        }();
        return value;
    }
    static bool Trace() {
        static const bool value = std::getenv("CCCASTER_OFFLINE_PACING_TRACE") != nullptr;
        return value;
    }
    struct Sample {
        uint32_t frame;
        int64_t due, waitQpc, waitAudio, tailQpc, ppm;
    };
    inline static int64_t sleepBegin = 0, sleepEnd = 0, sleepRemaining = 0;
    static void BeforeSleep(int64_t remaining) {
        if (!Trace()) return;
        sleepRemaining = remaining;
        sleepBegin = platform::RealMonotonicTicks();
    }
    static void AfterSleep() {
        if (Trace()) sleepEnd = platform::RealMonotonicTicks();
    }
    inline static Sample sample{};
    inline static bool pending = false;
    static void Waited(uint32_t frame, int64_t due) {
        if (!Trace()) return;
        WasapiClock::ReadSample clock;
        WasapiClock::GetTimeTicks(&clock);
        sample = {frame, due, clock.qpc, clock.time, 0, clock.ppm};
    }
    static void Prepared() {
        if (!Trace()) return;
        sample.tailQpc = platform::RealMonotonicTicks();
        pending = true;
    }
    static void Flush() {
        if (!pending) return;
        pending = false;
        domain::session::DebugLog("[OfflinePacing] f=%u due=%lld waitQpc=%lld waitAudio=%lld tailQpc=%lld ppm=%lld sleepBegin=%lld sleepEnd=%lld sleepRemaining=%lld",
            sample.frame, sample.due, sample.waitQpc, sample.waitAudio, sample.tailQpc, sample.ppm,
            sleepBegin, sleepEnd, sleepRemaining);
    }
};
}
