#include "core_dll/timing/UpdateCadence.hpp"
#include "core_dll/timing/ClockContinuity.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/mbaa_mem/BattleProgress.hpp"
#include "core_dll/sync/PostRoundDelay.hpp"
#include <cstdio>
#include <cstdlib>
void require(bool ok) {
    if (!ok)
        std::abort();
}
int main() {
    cccaster::core::sync::PostRoundDelay postRound;
    require(postRound.Source(100) == 100);
    postRound.Begin(100, 4);
    for (uint32_t f = 95; f != 120; ++f) {
        require(postRound.Source(f) == (f < 100 ? f : f < 104 ? 99 : f - 4));
        require(postRound.Source(f + 1) >= postRound.Source(f));
    }
    postRound.Begin(110, 8); // 同じ決着区間の途中で基準を動かさない。
    require(postRound.Source(110) == 106);
    postRound.Reset();
    postRound.Begin(131073, 0);
    require(postRound.Source(131074) == 131074 && !postRound.first);
    using namespace cccaster::core::timer;
    using namespace cccaster::game_interface;
    require(ClassifyBattle(true, 2, true) == BattleProgress::CharacterIntro);
    require(ClassifyBattle(true, 1, true) == BattleProgress::PreFight);
    require(ClassifyBattle(true, 0, true) == BattleProgress::InputLocked);
    require(ClassifyBattle(true, 0, false) == BattleProgress::Fighting);
    require(AllowsRollback(ClassifyBattle(true, 2, true)));
    require(AllowsRollback(ClassifyBattle(true, 1, true)));
    require(!AllowsRollback(ClassifyBattle(true, 0, true)));
    require(!AllowsRollback(ClassifyBattle(false, 2, false)));
    require(!AllowsRollback(ClassifyBattle(true, 3, false)));
    // 速度の両端、丸めの両側、既に遅れた締切でも早期解放しない。
    for (int64_t ppm : {-1000, -1, 0, 1, 1000}) {
        for (int64_t fraction : {-999999, 0, 999999}) {
            ClockAnchor anchor{100000000, 200000000, fraction, ppm};
            const int64_t now = 100040000;
            for (int64_t delta : {-1, 0, 1, 60, 12000}) {
                const auto target = anchor.AtTicks(now) + delta;
                const auto due = anchor.DeadlineTicks(target, now);
                require(due >= now && anchor.AtTicks(due) >= target);
                require(due == now || anchor.AtTicks(due - 1) < target);
            }
        }
    }
    ClockContinuity healthy, stalled;
    for (int i = 0; i <= 1000; ++i) {
        auto q = 1000000LL + i * 1000;
        require(healthy.Read(q, 100000LL + i * 1000) == q);
        stalled.Read(q, 100000LL + i * 900);
    }
    require(!healthy.IsFallback());
    require(stalled.IsFallback());
    const auto before = stalled.Read(2100000, 1);
    require(stalled.Read(3100000, 90000000) - before == 1000000);
    ClockContinuity interrupted;
    interrupted.Read(1000000, 10000);
    require(interrupted.Read(1001000, 0) == 1001000);
    require(interrupted.IsFallback());
    require(interrupted.Read(2001000, 99999999) == 2001000);
    ClockContinuity noisy;
    noisy.Read(1000000, 10000);
    noisy.Read(1001000, 9000);
    require(noisy.Read(1002000, 12000) == 1002000); // 原点の足し直しによる加速を禁止。
    FrameTiming timing;
    timing.Observe(1000000, 100, false);
    timing.Observe(1001000, 100, true); // 再計算は表示にも前進にも数えない。
    timing.Observe(1016667, 101, false);
    require(timing.last == 16667 && timing.gameFps > 59.99 && timing.gameFps < 60.01);
    timing.Observe(1033333, 102, true); // 前進フレームの描画省略。
    timing.Observe(1050000, 103, false);
    require(timing.displayFps == 40.0 && timing.gameFps == 60.0);
    timing.Observe(1100000, 65536, false); // 世代切替を速度に混ぜない。
    require(timing.last == 0);
    FrameTiming immediate, deferred;
    int64_t sampleUs = 2000000;
    for (uint32_t frame = 100; frame < 500; ++frame) {
        sampleUs += 16660 + frame % 13;
        const bool skipped = frame % 7 == 0;
        const auto previousLast = deferred.last;
        const auto previousFps = deferred.gameFps;
        deferred.Capture(sampleUs, frame, skipped);
        require(deferred.last == previousLast && deferred.gameFps == previousFps);
        const bool report = immediate.Observe(sampleUs, frame, skipped);
        require(deferred.ObserveCaptured() == report);
        require(deferred.last == immediate.last && deferred.minimum == immediate.minimum &&
                deferred.maximum == immediate.maximum && deferred.displayFps == immediate.displayFps &&
                deferred.gameFps == immediate.gameFps && deferred.Skips() == immediate.Skips());
        require(!deferred.ObserveCaptured()); // 再計算Presentでは同じ標本を追加しない。
    }
    deferred.Capture(sampleUs + 16667, 500, false);
    deferred.Reset(); // Device Reset/場面切替で保留中の旧世代標本を破棄する。
    require(!deferred.ObserveCaptured() && deferred.last == 0 && deferred.Skips() == 0);
    deferred.Capture(10000000, 65536, false);
    require(!deferred.ObserveCaptured() && deferred.last == 0);
    deferred.Capture(10016667, 65537, false);
    require(!deferred.ObserveCaptured() && deferred.last == 16667);
    deferred.Capture(11000000, 131072, false); // 採取したフレームの世代切替も従来どおり。
    require(!deferred.ObserveCaptured() && deferred.last == 0);
    cccaster::diagnostics::UpdateCadence cadence;
    cccaster::diagnostics::UpdateCadence::Sample sample;
    cadence.Arm(100); cadence.Capture(1000000); require(cadence.Take(sample));
    require(!sample.consecutive && !sample.spike);
    cadence.Capture(1500000); require(!cadence.Take(sample)); // 非Arm再計算は標本にしない。
    cadence.Arm(101); cadence.Capture(2000180); require(cadence.Take(sample));
    require(sample.consecutive && sample.error == 180 && !sample.spike);
    cadence.Arm(102); cadence.Capture(3000361); require(cadence.Take(sample));
    require(sample.error == 181 && sample.spike); // 最大値・境界の1tick超過。
    cadence.Arm(103); cadence.Capture(4000180); require(cadence.Take(sample));
    require(sample.error == -181 && sample.spike);
    cadence.Arm(104, false); cadence.Capture(9000000); require(cadence.Take(sample));
    cadence.Arm(105); cadence.Capture(15000000); require(cadence.Take(sample));
    require(!sample.consecutive && !sample.spike);
    cadence.Arm(131073); cadence.Capture(20000000); require(cadence.Take(sample));
    require(!sample.consecutive);
    cadence.Arm(131074); cadence.Capture(21000000);
    cadence.Arm(131075); cadence.Capture(22000000);
    require(cadence.dropped == 1 && cadence.Take(sample));
    std::puts("clock continuity / frame timing / update cadence OK");
}
