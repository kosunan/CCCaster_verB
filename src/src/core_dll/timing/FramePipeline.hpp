#pragma once
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <cstdlib>

namespace cccaster::diagnostics {
// Game-thread diagnostic only. Keep native entry and final rendering visible even
// when the frame cadence starts before input preparation. Replay keeps this sample.
struct FramePipeline {
    struct Sample {
        uint32_t frame;
        int64_t start, input, saved, prepared, native, present;
    };
    inline static Sample sample{};
    static bool Enabled() {
        static const bool enabled = std::getenv("CCCASTER_UPDATE_CADENCE") != nullptr;
        return enabled;
    }
    static void Begin(uint32_t frame, int64_t ticks) {
        if (Enabled()) sample = {frame, ticks};
    }
    static void InputRead() { if (sample.start) sample.input = platform::RealMonotonicTicks(); }
    static void Saved() { if (sample.start) sample.saved = platform::RealMonotonicTicks(); }
    static void Prepared() { if (sample.start) sample.prepared = platform::RealMonotonicTicks(); }
    static void NativeEntered() {
        if (sample.start && !sample.native) sample.native = platform::RealMonotonicTicks();
    }
    static void PresentReady() { if (sample.start) sample.present = platform::RealMonotonicTicks(); }
    static void Flush() {
        if (!sample.start) return;
        const auto s = sample;
        sample = {};
        domain::session::DebugLog("[FramePipeline] f=%u start=%lld input=%lld saved=%lld prepared=%lld native=%lld present=%lld",
            s.frame, s.start, s.input, s.saved, s.prepared, s.native, s.present);
    }
};
}
