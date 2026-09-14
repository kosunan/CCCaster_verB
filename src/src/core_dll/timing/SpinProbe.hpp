#pragma once
#include <algorithm>
#include <cstdlib>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DeferredNumericLog.hpp"
namespace cccaster::diagnostics {
struct SpinClockMetrics {
    int64_t lastQpc = 0, lastTime = 0, lastAge = 0;
    int64_t innerGap = 0, audioGap = 0, previousAge = 0;
    int64_t maxAge = 0, ageLate = 0, maxShift = 0, maxClamp = 0, ppm = 0;
    int64_t maxAgeBegin = 0, maxAgeEnd = 0;
    void Observe(int64_t qpc, int64_t time, int64_t after, int64_t due,
                 int64_t shift, int64_t clamp, int64_t rate) {
        if (lastQpc) {
            innerGap = qpc - lastQpc;
            audioGap = time - lastTime;
            previousAge = lastAge;
        }
        lastAge = after - qpc;
        if (lastAge > maxAge) {
            maxAge = lastAge; ageLate = time - due;
            maxAgeBegin = qpc; maxAgeEnd = after;
        }
        if (std::abs(shift) > std::abs(maxShift)) maxShift = shift;
        maxClamp = std::max(maxClamp, clamp);
        lastQpc = qpc; lastTime = time; ppm = rate;
    }
};
struct SpinMetrics {
    int64_t first = 0, lastAfter = 0;
    int64_t maxGap = 0, maxRead = 0, maxBetween = 0, gapEndLate = 0, exitLate = 0;
    int64_t gapRead = 0, gapBetween = 0, exitGap = 0, exitRead = 0, exitBetween = 0;
    int64_t maxGapBegin = 0, maxGapEnd = 0;
    unsigned reads = 0;
    void Observe(int64_t before, int64_t after, int64_t audio, int64_t due) {
        if (!reads) first = before;
        else {
            const auto gap = after - lastAfter;
            if (gap > maxGap) {
                maxGap = gap; gapEndLate = audio - due;
                gapRead = after - before; gapBetween = before - lastAfter;
                maxGapBegin = lastAfter; maxGapEnd = after;
            }
            maxBetween = std::max(maxBetween, before - lastAfter);
            exitGap = gap;
            exitBetween = before - lastAfter;
        }
        maxRead = std::max(maxRead, after - before);
        exitRead = after - before;
        lastAfter = after;
        exitLate = audio - due;
        ++reads;
    }
};
struct SpinProbe {
    struct Sample {
        uint32_t frame;
        uint32_t pid, tid;
        bool play;
        int64_t due, ready, bounded, waitReturn, begin, input, trace, commit, step, gameReturn;
        int64_t callbackEntry, observeBegin, observeEnd, callbackEnd;
        int64_t simEntry, retryEnd, stressEnd, gaugeEnd, effectsEnd;
        SpinMetrics spin;
        SpinClockMetrics clock;
    };
    inline static thread_local Sample sample{};
    inline static thread_local bool pending = false;
    struct InputWaitSample { uint32_t frame, reason; int64_t begin, end; };
    inline static thread_local InputWaitSample inputWaits[8]{};
    inline static thread_local unsigned inputWaitCount = 0, inputWaitDropped = 0;
    static void RecordInputWait(uint32_t frame, uint32_t reason, int64_t begin, int64_t end) {
        if (end-begin < 60000) return;
        if (inputWaitCount < 8) inputWaits[inputWaitCount++]={frame,reason,begin,end};
        else ++inputWaitDropped;
    }
    static bool Enabled() {
        static const bool enabled = std::getenv("CCCASTER_SPIN_PROBE") != nullptr;
        return enabled;
    }
    static int64_t Now() { return platform::RealMonotonicTicks(); }
    static void Start(uint32_t frame, bool play, int64_t due, int64_t ready) {
        sample = {};
        sample.frame = frame; sample.play = play; sample.due = due; sample.ready = ready;
        sample.pid = platform::ProcessId(); sample.tid = platform::ThreadId();
        pending = true;
        DeferredNumericLog::Prepare();
    }
    static void Flush() {
        if (Enabled()) {
            for (unsigned i=0; i<inputWaitCount; ++i) {
                const auto &w=inputWaits[i];
                domain::session::DebugLog("[InputWait] f=%u reason=%u begin=%lld end=%lld pid=%u tid=%u",
                    w.frame,w.reason,w.begin,w.end,platform::ProcessId(),platform::ThreadId());
            }
            if (inputWaitDropped) domain::session::DebugLog("[InputWaitDropped] count=%u",inputWaitDropped);
            inputWaitCount=inputWaitDropped=0;
        }
        if (!pending) return;
        DeferredNumericLog::Flush();
        const auto &s = sample;
        const auto &m = s.spin;
        domain::session::DebugLog("[SpinProbe] f=%u play=%d due=%lld ready=%lld reads=%u gap=%lld read=%lld between=%lld gapLate=%lld exitLate=%lld",
            s.frame, int(s.play), s.due, s.ready, m.reads, m.maxGap, m.maxRead, m.maxBetween, m.gapEndLate, m.exitLate);
        domain::session::DebugLog("[SpinTail] f=%u spin=%lld bounded=%lld wait=%lld begin=%lld input=%lld trace=%lld commit=%lld step=%lld game=%lld",
            s.frame, m.lastAfter, s.bounded, s.waitReturn, s.begin, s.input, s.trace, s.commit, s.step, s.gameReturn);
        domain::session::DebugLog("[SpinGap] f=%u first=%lld gapRead=%lld gapBetween=%lld exitGap=%lld exitRead=%lld exitBetween=%lld",
            s.frame, m.first, m.gapRead, m.gapBetween, m.exitGap, m.exitRead, m.exitBetween);
        domain::session::DebugLog("[SpinReturn] f=%u entry=%lld observeBegin=%lld observeEnd=%lld callbackEnd=%lld",
            s.frame, s.callbackEntry, s.observeBegin, s.observeEnd, s.callbackEnd);
        domain::session::DebugLog("[SpinBegin] f=%u entry=%lld retry=%lld stress=%lld gauge=%lld effects=%lld",
            s.frame, s.simEntry, s.retryEnd, s.stressEnd, s.gaugeEnd, s.effectsEnd);
        const auto &c = s.clock;
        domain::session::DebugLog("[SpinClock] f=%u innerGap=%lld audioGap=%lld prevAge=%lld age=%lld maxAge=%lld ageLate=%lld shift=%lld clamp=%lld ppm=%lld",
            s.frame, c.innerGap, c.audioGap, c.previousAge, c.lastAge, c.maxAge, c.ageLate,
            c.maxShift, c.maxClamp, c.ppm);
        domain::session::DebugLog("[SpinOS] f=%u pid=%u tid=%u maxGapBegin=%lld maxGapEnd=%lld maxAgeBegin=%lld maxAgeEnd=%lld",
            s.frame, s.pid, s.tid, m.maxGapBegin, m.maxGapEnd, c.maxAgeBegin, c.maxAgeEnd);
        pending = false;
    }
};
} // namespace cccaster::diagnostics
