#include "core_dll/timing/UpdateCadence.hpp"
#include "core_dll/spectator/Playback.hpp"
#include <cstdio>
#include <string>
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/ui/ScoreBroadcast.hpp"
#include "core_dll/timing/SpinProbe.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/timing/OfflinePacing.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
// 通常フレームは確定入力または上限内の予測を適用し、訂正時は保存状態から再計算する。
// 2026-09-10 ユーザー承認: ゲームスレッドの期限付き待機を許可。
#include "shared_contracts/NetplaySettings.hpp"
#include "shared_contracts/SpikeDebugGate.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/engine/MatchContext.hpp"
#include "core_dll/engine/FrameControl.hpp"
#include "core_dll/engine/SceneFastBoot.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/engine/SceneInputFilter.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/InputTrace.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/sync/InputTimeline.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/sync/FrameSequence.hpp"
#include "core_dll/sync/PostRoundDelay.hpp"
#include "core_dll/sync/BoundedWait.hpp"
#include "core_dll/mbaa_mem/GamePhaseDetector.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaMemTrace.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#ifdef _WIN32
#include "core_dll/hook/WndProcHook.hpp"
#endif
#include "shared_contracts/IpcData.hpp"
#include "core_dll/rollback/PredictionHistory.hpp"
#include "core_dll/rollback/RollbackStates.hpp"
#include <cstdlib>
#include "core_dll/engine/RematchChoice.hpp"
#include "core_dll/engine/LocalInputGate.hpp"
#include "core_dll/engine/RetryInputGate.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"

namespace cccaster::domain::session {
using namespace cccaster::core::sync;
using Session = cccaster::core::netplay::NetplaySession;
using GamePhase = cccaster::game_interface::GamePhase;
using GameInput = cccaster::game_interface::GameInput;
using Error = cccaster::public_api::SessionErrorType;
namespace {
struct SceneRuntime {
    cccaster::spectator::Playback spectator;
    bool broadcasting = false;
    bool spectatorSelectionSent = false;
    uint32_t spectatorNext = 0;
    cccaster::spectator::Record spectatorPending;
    bool spectatorHasPending = false;
    SessionScore score;
    FrameAdvantageTracker advantage;
    FrameBarHistory frameBar;
    TrainingState trainingState;
    MatchContext *context = nullptr;
    scene::LocalInputGate localInputGate, secondInputGate;
    std::atomic<bool> ready{false};
    bool running = false, syncedReported = false, haveWorld = false;
    GamePhase previous = GamePhase::Unknown;
    uint8_t previousIntro = 255;
    FrameSequence sequence;
    PostRoundDelay postRoundDelay;
    uint32_t waitCount = 0, lastWorld = 0;
    cccaster::sync::PredictionHistory history;
    cccaster::sync::RollbackStates snapshots;
    uint32_t replayFrame = 0, replayTarget = 0, lastForced = 0;
    bool snapshotReady = false;
    uint32_t retryDriveFrames = 0;
    bool fastRematchTransition = false;
    int64_t rematchTransitionStarted = 0;
    int64_t replayStarted = 0;
    int64_t replayBeginTicks = 0, replayRestoreEnd = 0, replaySaveTicks = 0, replayPrepareTicks = 0;
    uint32_t replaySaved = 0, replaySkipped = 0;
    uint32_t replayFrom = 0;
    int64_t selectionStarted = 0;
    bool selectionReleased = false;
    int64_t selectionTick = 0;
    scene::RetryInputGate retryInputGate;
    int64_t retryTick = 0;
};
SceneRuntime runtime;

void PublishSpectatorConfirmed() {
    if (!runtime.broadcasting || !runtime.spectatorNext) return;
    if (runtime.snapshotReady ? runtime.spectatorNext > runtime.history.Confirmed() : !runtime.spectatorHasPending) return;
    static const bool profile = std::getenv("CCCASTER_SPECTATOR_PROFILE") != nullptr;
    struct Measure {
        int64_t begin;
        ~Measure() {
            if (!begin) return;
            const auto elapsed = cccaster::platform::RealMonotonicTicks() - begin;
            static int64_t total = 0, maximum = 0;
            static uint32_t count = 0;
            total += elapsed; maximum = std::max(maximum, elapsed);
            if (++count % 300 == 0)
                DebugLog("[SpectatorPublish] count=%u ticks=%lld maxTicks=%lld hz=60000000", count, total, maximum);
        }
    } measure{profile ? cccaster::platform::RealMonotonicTicks() : 0};
    auto &wire = cccaster::spectator::Transport::Get();
    if (!wire.Active()) { runtime.broadcasting = false; return; }
    if (runtime.snapshotReady) {
        // 最大R+1件。訂正再計算が終了してから公開し、重複送信しない。
        for (; runtime.spectatorNext <= runtime.history.Confirmed(); ++runtime.spectatorNext) {
            const auto *input = runtime.history.Get(runtime.spectatorNext);
            if (!input) { runtime.broadcasting = false; return; }
            if (!wire.PublishInput(runtime.spectatorNext, input->local & SettingsCommands::GameMask,
                                   input->remote & SettingsCommands::GameMask)) {
                runtime.broadcasting = false; return;
            }
        }
    } else if (runtime.spectatorHasPending) {
        wire.PublishInput(runtime.spectatorPending.frame, runtime.spectatorPending.payload[0], runtime.spectatorPending.payload[1]);
        runtime.spectatorHasPending = false;
        ++runtime.spectatorNext;
    }
}

void WriteGameInputs(GamePhase phase, uint32_t p1, uint32_t p2) {
    auto &mem = cccaster::game_interface::GameMem();
    if (phase == GamePhase::Rematch) {
        const int before = scene::rematchChoice.result;
        scene::rematchChoice.Step(GameInput::Unpack(p1), GameInput::Unpack(p2));
        const int result = scene::rematchChoice.result;
        if (before < 0 && result >= 0) {
            mem.SetRetryTarget(result);
            InputTimeline::GetInstance().Pause();
            runtime.fastRematchTransition = true;
            runtime.rematchTransitionStarted = cccaster::platform::RealMonotonicUs();
            FrameControl::SetModeHighSpeedSkip();
            DebugLog("[Rematch] FAST ON target=%d", result);
            DebugLog("[Rematch] RESOLVED frame=%u p1=%d p2=%d target=%d", runtime.sequence.Next(),
                     scene::rematchChoice.players[0].choice, scene::rematchChoice.players[1].choice, result);
        }
        // UIで選択している間はゲーム本来のメニューに操作を渡さない。
        // 決定後は8フレーム間隔で同じ操作を送り、実メニュー処理直前のフックで選択先を固定する。
        GameInput nav{};
        if (result >= 0 && ++runtime.retryDriveFrames % 8 == 0)
            nav.buttons = CC_BUTTON_A | CC_BUTTON_CONFIRM;
        FrameControl::WriteInput(nav, nav);
    } else
        FrameControl::WriteInput(GameInput::Unpack(p1), GameInput::Unpack(p2));
}

void TraceFrame(uint32_t frame, GameInput p1, GameInput p2) {
    auto &mem = cccaster::game_interface::GameMem();
    if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled()) {
        cccaster::diagnostics::DeferredNumericLog::Log("[REC] %u %u %u %u %u", frame, p1.direction, p1.buttons, p2.direction, p2.buttons);
        cccaster::diagnostics::DeferredNumericLog::Log("[FRAME] %u %u %u %u %u", frame,
                 static_cast<unsigned>(cccaster::game_interface::PhaseMonitor::GetCurrentPhase()),
                 mem.IntroState(), mem.WorldTimer() - runtime.context->phaseBaseWorldTimer, mem.RealTimer());
    }
    cccaster::game_memory::MbaaMemTrace::Sample(frame);
}

void Fail(Error error, const char *reason) {
    cccaster::domain::ui::score_broadcast::Clear();
    InputTimeline::GetInstance().Pause();
    runtime.running = false;
    runtime.fastRematchTransition = false;
    FrameControl::SetModeNormalSpeed();
    DebugLog("[InputGate] FAILED reason=%s frame=%u WT=%u", reason, runtime.sequence.Next(),
             cccaster::game_interface::GameMem().WorldTimer());
    // IpcManagerの関数ポインタAPIへ同一ゲームスレッドの一時値を渡す。
    static uint32_t code;
    code = static_cast<uint32_t>(error);
    cccaster::public_api::IpcManager::UpdateOrReadState([](cccaster::public_api::SharedState &s) {
        s.syncCompleted = false;
        if (s.lastErrorCode != static_cast<uint32_t>(Error::PeerClosed)) s.lastErrorCode = code;
    });
    FrameControl::ExitGame();
}

bool AbortRequested() {
#ifdef _WIN32
    // 入力待機中はWndProcまで到達しないので、前面にある自分のゲームだけを確認する。
    if (runtime.context && runtime.context->appMode == 0 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) &&
        !cccaster::game_interface::WndProcHook::BlocksEscapeExit() &&
        !cccaster::domain::ui::StateUiLogic::IsMappingWindowOpen()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId())
            cccaster::public_api::RequestLocalGameExit(cccaster::public_api::SessionExitReason::Escape);
    }
#endif
    return cccaster::platform::IsAbortRequested();
}

template <class Predicate>
bool Wait(const char *reason, int64_t timeoutUs, Predicate predicate, int64_t deadlineTicks = 0,
          int64_t readyHintTicks = 0) {
    const int64_t started = cccaster::platform::RealMonotonicUs();
    const uint32_t beforeWT = cccaster::game_interface::GameMem().WorldTimer();
    static const bool stageTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    static const bool inlineDeadlineTrace = std::getenv("CCCASTER_INLINE_DEADLINE_TRACE") != nullptr;
    const bool simulationWait = deadlineTicks && !std::strcmp(reason, "simulation deadline");
    int64_t spinStart = 0, spinEnd = 0;
    using Probe = cccaster::diagnostics::SpinProbe;
    const bool probe = deadlineTicks && Probe::Enabled() && Probe::pending;
    const unsigned waitReason = !deadlineTicks && Probe::Enabled()
        ? (!std::strcmp(reason,"remote input") ? 1 : !std::strcmp(reason,"metronome input") ? 2 :
           !std::strcmp(reason,"rollback boundary drain") ? 3 : 0) : 0;
    const auto inputWaitBegin = waitReason ? Probe::Now() : 0;
    const auto result = WaitBounded(
        timeoutUs,
        [&] {
            const auto hintNow = readyHintTicks ? cccaster::core::timer::WasapiClock::GetTimeTicks() : 0;
            if (readyHintTicks && hintNow >= readyHintTicks - 60000 && hintNow < readyHintTicks + 60000) {
                // 入力公開直前に500usのOS待機を繰り返すと、公開済みでも起床待ちになる。
                // 最大2msで外側の中断/疎通/期限検査へ戻り、入力未着を無期限スピンしない。
                const auto until = readyHintTicks + 60000;
                do {
                    if (predicate())
                        return true;
                    cccaster::platform::CpuRelax();
                } while (cccaster::core::timer::WasapiClock::GetTimeTicks() < until);
                return false;
            }
            if (deadlineTicks &&
                deadlineTicks - cccaster::core::timer::WasapiClock::GetTimeTicks() <=
                    60 * cccaster::core::timer::FrameTiming::SimulationSpinGuardUs) {
                spinStart = stageTrace ? cccaster::platform::RealMonotonicUs() : 0;
                if (probe) {
                    auto &sample = Probe::sample;
                    for (;;) {
                        const auto before = Probe::Now();
                        cccaster::core::timer::WasapiClock::ReadSample reading;
                        const auto audio = cccaster::core::timer::WasapiClock::GetTimeTicks(&reading);
                        const auto after = Probe::Now();
                        sample.spin.Observe(before, after, audio, deadlineTicks);
                        sample.clock.Observe(reading.qpc, audio, after, deadlineTicks,
                                             reading.anchorShift, reading.clamp, reading.ppm);
                        if (audio >= deadlineTicks) break;
                        cccaster::platform::CpuRelax();
                    }
                } else {
                    while (cccaster::core::timer::WasapiClock::GetTimeTicks() < deadlineTicks)
                        cccaster::platform::CpuRelax();
                }
                spinEnd = stageTrace ? cccaster::platform::RealMonotonicUs() : 0;
            }
            return predicate();
        },
        AbortRequested,
        [] {
            const auto &state = Session::GetState();
            return Session::GetInstance().IsRunning() &&
                   !state.protocolError.load(std::memory_order_acquire) &&
                   (!state.isSynced.load(std::memory_order_acquire) ||
                    state.isPeerAlive.load(std::memory_order_acquire));
        },
        cccaster::platform::RealMonotonicUs,
        [deadlineTicks, readyHintTicks] {
            const auto remaining =
                deadlineTicks ? (deadlineTicks - cccaster::core::timer::WasapiClock::GetTimeTicks()) / 60
                              : 500;
            // 更新締切の最後の3msはOSタイマーの再起床誤差を持ち込まない。
            if ((deadlineTicks && remaining <= cccaster::core::timer::FrameTiming::SimulationSpinGuardUs) ||
                (readyHintTicks &&
                 cccaster::core::timer::WasapiClock::GetTimeTicks() >= readyHintTicks - 60000 &&
                 cccaster::core::timer::WasapiClock::GetTimeTicks() < readyHintTicks + 60000))
                cccaster::platform::CpuRelax();
            else
                cccaster::platform::PreciseWaitUs(std::min<int64_t>(500, remaining));
        });
    if (probe) Probe::sample.bounded = Probe::Now();
    if (waitReason) Probe::RecordInputWait(runtime.sequence.Next(),waitReason,inputWaitBegin,Probe::Now());
    if (stageTrace && deadlineTicks && !probe)
        cccaster::diagnostics::DeferredNumericLog::Log("[SpinStage] f=%u begin=%lld end=%lld late=%lld", runtime.sequence.Next(), spinStart,
                 spinEnd, (cccaster::core::timer::WasapiClock::GetTimeTicks() - deadlineTicks) / 60);
    const auto elapsed = cccaster::platform::RealMonotonicUs() - started;
    if (elapsed >= 1000) {
        ++runtime.waitCount;
        if (!probe && (elapsed >= 20000 || runtime.waitCount % 60 == 1)) {
            const bool measure = simulationWait && cccaster::diagnostics::UpdateCadence::Enabled();
            const auto begin = measure ? Probe::Now() : 0;
            if (simulationWait && !inlineDeadlineTrace)
                cccaster::diagnostics::DeferredNumericLog::Log(
                    "[InputGate] WAIT reason=simulation deadline frame=%u elapsedUs=%lld WT=%u->%u result=%d",
                    runtime.sequence.Next(), elapsed, beforeWT,
                    cccaster::game_interface::GameMem().WorldTimer(), static_cast<int>(result));
            else
                DebugLog("[InputGate] WAIT reason=%s frame=%u elapsedUs=%lld WT=%u->%u result=%d", reason,
                         runtime.sequence.Next(), elapsed, beforeWT,
                         cccaster::game_interface::GameMem().WorldTimer(), static_cast<int>(result));
            if (measure) cccaster::diagnostics::DeferredNumericLog::Log(
                "[DeadlineTraceCost] f=%u begin=%lld end=%lld inline=%d",
                runtime.sequence.Next(), begin, Probe::Now(), int(inlineDeadlineTrace));
        }
    }
    if (result == WaitResult::Ready) {
        if (beforeWT != cccaster::game_interface::GameMem().WorldTimer()) {
            Fail(Error::SyncTimeout, "game advanced while waiting");
            return false;
        }
        if (probe) Probe::sample.waitReturn = Probe::Now();
        return true;
    }
    Fail(result == WaitResult::Cancelled      ? Error::AbortedByUser
         : result == WaitResult::Disconnected ? Error::PeerDisconnected
                                              : Error::SyncTimeout,
         reason);
    return false;
}
} // namespace

void SceneRunner::Init(MatchContext &ctx) {
    cccaster::domain::ui::StateUiLogic::ResetUtilityMetrics(ctx.appMode == 0);
    runtime.ready.store(false);
    runtime.score.Reset(ctx.appMode == 0);
    runtime.advantage.Reset();
    runtime.frameBar.Reset();
    runtime.trainingState.Reset();
    cccaster::domain::ui::score_broadcast::Publish(runtime.score.Snapshot());
    cccaster::core::sync::SettingsCommands::Reset(ctx.delay, ctx.maxRollback);
    runtime.context = &ctx;
    runtime.fastRematchTransition = false;
    runtime.localInputGate = {};
    runtime.secondInputGate = {};
    runtime.running = true;
    runtime.sequence = FrameSequence{};
    runtime.replayFrame = runtime.replayTarget = runtime.lastForced = 0;
    runtime.snapshotReady = false;
    runtime.previous = GamePhase::Unknown;
    runtime.previousIntro = 255;
    runtime.syncedReported = false;
    runtime.haveWorld = false;
    runtime.waitCount = 0;
    if (ctx.appMode == 0) {
        Session::GetInstance().Start(ctx.isHost, ctx.peerIp, ctx.peerPort, ctx.localPort, ctx.delay,
                                     ctx.maxRollback, ctx.playerName);
        runtime.broadcasting = ctx.isHost && ctx.localPort && !std::getenv("CCCASTER_SPECTATE_OFF");
        if (runtime.broadcasting) cccaster::spectator::Transport::Get().StartHost(ctx.localPort);
    } else if (ctx.appMode == 2) {
        cccaster::spectator::Transport::Get().StartViewer(ctx.peerIp, ctx.peerPort);
    }
    if (cccaster::diagnostics::startup::Baseline()) FrameControl::SetModeNormalSpeed();
    else FrameControl::SetModeHighSpeedSkip();
    scene::SceneFastBoot::Start(ctx.appMode == 2 ? cccaster::public_api::IpcGameMode::Versus
                                               : static_cast<cccaster::public_api::IpcGameMode>(ctx.appMode));
    runtime.ready.store(true, std::memory_order_release);
    DebugLog("[SceneRunner] rollback session initialized role=%s", ctx.isHost ? "host" : "client");
}

void SceneRunner::FlushCadence() {
    using Cadence = cccaster::diagnostics::UpdateCadence;
    Cadence::Sample s;
    auto &cadence = Cadence::Get();
    if (!cadence.Take(s)) return;
    const auto &release = cccaster::core::timer::WasapiClock::releaseSample;
    DebugLog("[ReleaseGate] f=%u ready=%lld due=%lld exit=%lld actual=%lld readyLate=%lld ppm=%lld",
             s.frame, release.ready, release.due, release.exit, s.now, release.readyLate, release.ppm);
    cccaster::diagnostics::DeferredNumericLog::Flush();
    DebugLog("[UpdateCadence] n=%u f=%u prev=%u ticks=%lld interval=%lld error=%lld consecutive=%d spike=%d dropped=%u evidence=%d play=%d",
             s.ordinal, s.frame, s.previous, s.now, s.interval, s.error, int(s.consecutive),
             int(s.spike), cadence.dropped, int(Cadence::Evidence()), int(s.play));
    if (!s.spike || !Cadence::Evidence()) return;
    // Present完了後、次のStep（保存・復元・Reset）より前。同じスレッドなのでリングと競合しない。
    // 出力コストは次周期に影響し得る。証拠採取モードの性能を通常性能にはしない。
    const auto begin = cccaster::platform::RealMonotonicTicks();
    const std::string path = std::string(std::getenv("CCCASTER_SPIKE_STATE_DIR")) +
        "/spike_" + std::to_string(cccaster::platform::ProcessId()) + "_" +
        std::to_string(s.ordinal) + ".bin";
    FILE *file = std::fopen(path.c_str(), "wb");
    bool ok = file != nullptr;
    auto write = [&](const void *p, size_t size) {
        if (ok && std::fwrite(p, 1, size, file) != size) ok = false;
    };
    const uint32_t header[] = {0x53504343, 1, s.frame, s.previous, s.ordinal,
        cccaster::platform::ProcessId(), 10, sizeof(std::fenv_t), runtime.history.Confirmed(),
        runtime.history.Next(), uint32_t(runtime.context && runtime.context->isHost), uint32_t(Cadence::Limit)};
    const int64_t times[] = {s.now, s.interval, s.error};
    write(header, sizeof(header)); write(times, sizeof(times));
    unsigned present = 0;
    for (unsigned back = 10; back; --back) {
        const uint32_t f = s.frame >= back ? s.frame - back : 0;
        const auto *input = runtime.history.Get(f);
        bool found = false;
        if (f / 65536 == s.frame / 65536) {
            found = runtime.snapshots.Export(f, [&](const auto &bytes, const auto &fp, uint32_t local, uint32_t remote) {
                const uint32_t row[] = {f, 1, uint32_t(bytes.size()), local, remote,
                    input ? 1u : 0u, input ? input->local : 0, input ? input->remote : 0};
                write(row, sizeof(row)); write(&fp, sizeof(fp)); write(bytes.data(), bytes.size());
                return true;
            });
        }
        if (found) ++present;
        else {
            const uint32_t row[] = {f, 0, 0, 0, 0, input ? 1u : 0u,
                input ? input->local : 0, input ? input->remote : 0};
            write(row, sizeof(row));
        }
    }
    if (file && std::fclose(file)) ok = false;
    DebugLog("[SpikeState] f=%u n=%u complete=%d present=%u ok=%d begin=%lld end=%lld file=%s",
        s.frame, s.ordinal, int(present == 10), present, int(ok), begin,
        cccaster::platform::RealMonotonicTicks(), path.c_str());
}

void SceneRunner::Step() {
    using OfflinePacing = cccaster::core::timer::OfflinePacing;
    OfflinePacing::Flush();
    // harnessも同じ入口を通る。前更新の数値を次の締切準備前に回収する。
    cccaster::diagnostics::DeferredNumericLog::Flush();
    cccaster::core::timer::FrameTiming::presentDueTicks = 0;
    if (!runtime.ready.load(std::memory_order_acquire) || !runtime.running || !runtime.context)
        return;
    if (runtime.context->appMode == 0 ||
        (runtime.context->appMode != 2 && OfflinePacing::Mode() == OfflinePacing::Variant::Normal)) {
        thread_local cccaster::platform::TimingThread priority("game");
        priority.MaintainAffinity();
    }
    auto &ctx = *runtime.context;
    auto &mem = cccaster::game_interface::GameMem();
    if (AbortRequested()) {
        Fail(Error::AbortedByUser, "abort");
        return;
    }
    if (!mem.IsAvailable()) {
        Fail(Error::SyncTimeout, "game memory unavailable");
        return;
    }
    if ((ctx.appMode == 0 || ctx.appMode == 2) && !mem.ConfigureNetplayMenu()) {
        Fail(Error::SyncTimeout, "netplay menu hook unavailable");
        return;
    }
    auto &inputBuffer = MatchInputBuffer::GetInstance();
    const auto earlyPhase = cccaster::game_interface::PhaseMonitor::GetCurrentPhase();
    if (ctx.appMode == 2) {
        if (!scene::SceneFastBoot::IsComplete()) {
            scene::SceneFastBoot::ProcessFrame(true);
            if (!scene::SceneFastBoot::IsComplete()) return;
        }
        if (earlyPhase == GamePhase::InGame) {
            const auto sample = mem.ReadTrainingFrame();
            runtime.advantage.Update(2, sample);
            runtime.frameBar.Update(2, sample);
        } else { runtime.advantage.Reset(); runtime.frameBar.Reset(); }
        const auto before = runtime.spectator.Last();
        if (!runtime.spectator.Step(earlyPhase)) {
            const bool closed = cccaster::spectator::Transport::Get().State() == cccaster::spectator::Status::Disconnected;
            Fail(Error::PeerDisconnected, closed ? "spectator connection closed" : "spectator stream invalid or timed out"); return;
        }
        const auto frame = runtime.spectator.Last();
        if (frame != before) {
            if ((frame & 65535) == 1) ctx.phaseBaseWorldTimer = mem.WorldTimer();
            // 入力はPlaybackが書込み済み。メモリ診断は通常の比較器に渡す。
            TraceFrame(frame, GameInput::Unpack(runtime.spectator.inputs[0]), GameInput::Unpack(runtime.spectator.inputs[1]));
            if (cccaster::testing::IsInputTraceEnabled()) {
                DebugLog("[CONFIRMED] %u", frame);
                DebugLog("[Spectator] FRAME f=%u catch=%d buffered=%u qpc=%lld", frame, runtime.spectator.Catching(),
                         cccaster::spectator::Transport::Get().Buffered(), cccaster::platform::RealMonotonicUs());
            }
        }
        if (!runtime.syncedReported && cccaster::spectator::Transport::Get().State() >= cccaster::spectator::Status::Waiting) {
            runtime.syncedReported = true;
            cccaster::public_api::IpcManager::UpdateOrReadState([](cccaster::public_api::SharedState &s) { s.syncCompleted = true; });
        }
        return;
    }
    // 遷移先を検出した最初のPresentで解除。境界の通信待機よりも先に戻す。
    if (runtime.fastRematchTransition && earlyPhase != GamePhase::Rematch) {
        runtime.fastRematchTransition = false;
        FrameControl::SetModeNormalSpeed();
        DebugLog("[Rematch] FAST OFF phase=%u elapsedUs=%lld", static_cast<unsigned>(earlyPhase),
                 cccaster::platform::RealMonotonicUs() - runtime.rematchTransitionStarted);
    }
    auto &timeline = InputTimeline::GetInstance();
    if (ctx.appMode == 0 && !runtime.replayFrame &&
        (earlyPhase != runtime.previous ||
         (earlyPhase == GamePhase::InGame && mem.IntroState() == 2 && runtime.previousIntro != 2)))
        timeline.Pause();
reconcileBoundary:
    const auto readRemote = [&](uint32_t f, uint32_t &v) {
        return inputBuffer.TryGetRemoteInput(runtime.postRoundDelay.Source(f), v);
    };
    if (runtime.snapshotReady && !runtime.replayFrame) {
        const auto currentPhase = cccaster::game_interface::PhaseMonitor::GetCurrentPhase();
        if (!mem.CanRollback() || currentPhase != runtime.previous ||
            (mem.IntroState() == 2 && runtime.previousIntro != 2)) {
            if (!Wait("rollback boundary drain", 3000000, [&] {
                    for (uint32_t f = runtime.history.Confirmed() + 1; f < runtime.history.Next(); ++f) {
                        uint32_t v;
                        if (!readRemote(f, v))
                            return false;
                    }
                    return true;
                }))
                return;
        }
        uint32_t mismatch = runtime.history.Reconcile(readRemote);
        const uint32_t next = runtime.sequence.Next();
        static const bool introTest = std::getenv("CCCASTER_TEST_INTRO_ROLLBACK") != nullptr;
        // 新ラウンドのintro=2入口では旧世代へ強制的に戻さない。
        // 本番の境界drainを通した後、新世代内の保存が揃ってから試験する。
        const bool forceIntro = introTest && next != runtime.lastForced && mem.CanRollback() && next - runtime.sequence.Base() > 8 &&
            (mem.IntroState() != 2 || runtime.previousIntro == 2) &&
            (mem.IntroState() == 1 || mem.IntroState() == 2 || runtime.previousIntro == 1) &&
            (next - runtime.lastForced > 30 || mem.IntroState() != runtime.previousIntro);
        const bool forced = forceIntro || (ctx.isHost && std::getenv("CCCASTER_TEST_ROLLBACK") && mem.CanPredict() &&
                            cccaster::game_interface::PhaseMonitor::GetCurrentPhase() == GamePhase::InGame &&
                            next - runtime.sequence.Base() > 400 && next - runtime.lastForced > 180);
        if (forced) {
            mismatch = next - 4;
            runtime.lastForced = next;
        }
        if (mismatch) {
            const bool replayProbe = cccaster::diagnostics::SpinProbe::Enabled();
            if (replayProbe) cccaster::diagnostics::DeferredNumericLog::Prepare();
            runtime.replayBeginTicks = replayProbe ? cccaster::platform::RealMonotonicTicks() : 0;
            runtime.replaySaveTicks = runtime.replayPrepareTicks = 0;
            runtime.replaySaved = runtime.replaySkipped = 0;
            runtime.replayFrom = mismatch;
            const unsigned targetIntro = mem.IntroState();
            const auto restoreStart = cccaster::platform::RealMonotonicUs();
            if (!mem.BeginReplay(mismatch, runtime.sequence.Next()) ||
                !runtime.snapshots.Load(mismatch, mem)) {
                Fail(Error::SyncTimeout, "rollback restore failed");
                return;
            }
            if (std::getenv("CCCASTER_PACE_TRACE"))
                DebugLog("[RestoreStage] f=%u restore=%lld", runtime.sequence.Next(),
                         cccaster::platform::RealMonotonicUs() - restoreStart);
            runtime.replayRestoreEnd = replayProbe ? cccaster::platform::RealMonotonicTicks() : 0;
            runtime.replayFrame = mismatch;
            runtime.replayTarget = next;
            runtime.replayStarted = cccaster::platform::RealMonotonicUs();
            cccaster::domain::ui::StateUiLogic::RecordRollback(runtime.replayTarget - mismatch);
            if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
                cccaster::diagnostics::DeferredNumericLog::Log("[Rollback] BEGIN frame=%u target=%u depth=%u forced=%d intro=%u targetIntro=%u", mismatch, next,
                         next - mismatch, forced, unsigned(mem.IntroState()), targetIntro);
        }
    }
    if (runtime.replayFrame) {
        if (runtime.replayFrame == runtime.replayTarget) {
            const auto replayElapsedUs = cccaster::platform::RealMonotonicUs() - runtime.replayStarted;
            const auto replayEnd = runtime.replayBeginTicks ? cccaster::platform::RealMonotonicTicks() : 0;
            if (runtime.replayBeginTicks) {
                cccaster::diagnostics::DeferredNumericLog::Flush();
                DebugLog("[ReplayWork] f=%u from=%u begin=%lld restoreEnd=%lld end=%lld saves=%lld prepare=%lld pid=%u tid=%u saved=%u skipped=%u",
                    runtime.replayTarget, runtime.replayFrom, runtime.replayBeginTicks, runtime.replayRestoreEnd,
                    replayEnd, runtime.replaySaveTicks, runtime.replayPrepareTicks,
                    cccaster::platform::ProcessId(), cccaster::platform::ThreadId(), runtime.replaySaved, runtime.replaySkipped);
            }
            if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
                DebugLog("[Rollback] END target=%u WT=%u elapsedUs=%lld", runtime.replayTarget,
                         mem.WorldTimer(), replayElapsedUs);
            runtime.replayFrame = 0;
            mem.EndReplay();
            FrameControl::SetModeNormalSpeed();
            // 再計算そのものが終了画面へ達する場合も、未着末尾入力をdrainしてから
            // 遷移・集計へ進む。次のゲーム更新を挟まず、訂正があれば再び再計算する。
            goto reconcileBoundary;
        } else {
            const auto saveBegin = runtime.replayBeginTicks ? cccaster::platform::RealMonotonicTicks() : 0;
            uint32_t local, remote;
            // 強制ロールバック診断は確定済みフレームへ戻るため、従来の全保存を維持。
            static const bool keepConfirmed = std::getenv("CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS") ||
                                              std::getenv("CCCASTER_TEST_ROLLBACK") ||
                                              std::getenv("CCCASTER_TEST_INTRO_ROLLBACK") ||
                                              cccaster::diagnostics::UpdateCadence::Evidence();
            const bool save = keepConfirmed || runtime.history.NeedsReplaySnapshot(runtime.replayFrame);
            if (!runtime.history.ResolveReplay(runtime.replayFrame, readRemote, local, remote) ||
                (save && !runtime.snapshots.Save(runtime.replayFrame, mem, local, remote))) {
                Fail(Error::SyncTimeout, "rollback history missing");
                return;
            }
            if (save) ++runtime.replaySaved;
            else ++runtime.replaySkipped;
            const auto prepareBegin = runtime.replayBeginTicks ? cccaster::platform::RealMonotonicTicks() : 0;
            runtime.replaySaveTicks += prepareBegin - saveBegin;
            mem.BeginSimulation(runtime.replayFrame);
            FrameControl::SetModeHighSpeedSkip();
            FrameControl::WriteInput(GameInput::Unpack(ctx.isHost ? local : remote),
                                     GameInput::Unpack(ctx.isHost ? remote : local));
            TraceFrame(runtime.replayFrame, GameInput::Unpack(ctx.isHost ? local : remote),
                       GameInput::Unpack(ctx.isHost ? remote : local));
            if (runtime.replayBeginTicks)
                runtime.replayPrepareTicks += cccaster::platform::RealMonotonicTicks() - prepareBegin;
            ++runtime.replayFrame;
            return;
        }
    }
    PublishSpectatorConfirmed();
    if (runtime.snapshotReady) {
        Session::GetMutableState().consumedFrame.store(runtime.postRoundDelay.Source(runtime.history.Confirmed()),
                                                       std::memory_order_release);
        if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
            DebugLog("[CONFIRMED] %u", runtime.history.Confirmed());
    }
    const auto phase = cccaster::game_interface::PhaseMonitor::GetCurrentPhase();
    const auto intro = mem.IntroState();
    const auto world = mem.WorldTimer();
    // 直前までの予測は上のdrain/replayで確定済み。intro=1/2とは分離する。
    if (runtime.snapshotReady && phase == GamePhase::InGame && intro == 0 &&
        !mem.CanPredict() && !runtime.postRoundDelay.first) {
        runtime.postRoundDelay.Begin(runtime.sequence.Next(), ctx.maxRollback);
        DebugLog("[PostRoundDelay] first=%u extra=%u", runtime.postRoundDelay.first,
                 runtime.postRoundDelay.extra);
    }
    if (ctx.appMode == 1 || ctx.appMode == 2) {
        if (phase == GamePhase::InGame) {
            const auto sample = mem.ReadTrainingFrame();
            runtime.advantage.Update(ctx.appMode, sample);
            runtime.frameBar.Update(ctx.appMode, sample);
            static const bool traceTraining = std::getenv("CCCASTER_TRAINING_TRACE") != nullptr;
            if (traceTraining) {
                const auto result = runtime.advantage.Result();
                DebugLog("[TrainingAdvantage] valid=%u true=%u sim=%u round=%u active=%d/%d busy=%d/%d stopped=%u state=%u p1=%d",
                    unsigned(sample.valid), sample.trueFrame, sample.simulationFrame, sample.round,
                    sample.activeCharacter[0], sample.activeCharacter[1], sample.inactionable[0],
                    sample.inactionable[1], unsigned(sample.stopped), unsigned(result.state), result.p1Frames);
            }
        } else { runtime.advantage.Reset(); runtime.frameBar.Reset(); }
    }
    // 入力drainと訂正再計算を通過した結果だけ集計する。通常のラウンド勝利では増やさない。
    if (ctx.appMode == 0) {
        const auto scoreScene = phase == GamePhase::InGame ? ScoreScene::Battle
            : phase == GamePhase::Rematch ? ScoreScene::Result
            : phase == GamePhase::CharaSelect ? ScoreScene::CharacterSelect : ScoreScene::Other;
        const auto facts = scoreScene == ScoreScene::Result ? mem.ReadMatchResult() : MatchResultFacts{};
        if (runtime.score.Observe(scoreScene, runtime.sequence.Base(), facts, true)) {
            const auto score = runtime.score.Snapshot();
            if (runtime.broadcasting) {
                cccaster::spectator::Record record;
                record.Set(cccaster::spectator::Result, runtime.sequence.Next(),
                    cccaster::spectator::Score{score.p1Wins, score.p2Wins, score.unresolved, uint32_t(score.revision)});
                cccaster::spectator::Transport::Get().Publish(record);
            }
            cccaster::domain::ui::score_broadcast::Publish(score);
            DebugLog("[SessionScore] revision=%llu match=%llu p1=%u p2=%u unresolved=%u rounds=%u/%u required=%u",
                static_cast<unsigned long long>(score.revision), static_cast<unsigned long long>(score.lastMatchGeneration),
                score.p1Wins, score.p2Wins, score.unresolved, facts.p1Rounds, facts.p2Rounds, facts.roundsToWin);
        }
    }
    if (phase != runtime.previous) {
        DebugLog("[SceneRunner] Phase change: %d -> %d", static_cast<int>(runtime.previous),
                 static_cast<int>(phase));
        ctx.framesInPhase = 0;
        timeline.Pause();
        scene::SceneInputFilter::Reset();
        scene::rematchChoice.Reset();
        runtime.retryDriveFrames = 0;
        mem.SetRetryTarget(-1);
    }

    if (phase < GamePhase::CharaSelect && !scene::SceneFastBoot::IsComplete()) {
        scene::SceneFastBoot::ProcessFrame(ctx.isHost);
        runtime.previous = phase;
        runtime.previousIntro = intro;
        return;
    }
    if (phase == GamePhase::CharaSelect && !scene::SceneFastBoot::IsComplete())
        scene::SceneFastBoot::ProcessFrame(ctx.isHost);
    if (runtime.fastRematchTransition) {
        // 選択と合意は解決済み。この区間に新たな操作はなく、
        // 通信フレームを消費せず同じ自動決定操作だけで内部遷移を進める。
        // 旧世代のACKとkeepaliveは通信スレッドが送り続ける。
        FrameControl::SetModeHighSpeedSkip();
        if (mem.HasIndependentRetry()) {
            GameInput nav{};
            if (++runtime.retryDriveFrames % 8 == 0) nav.buttons = CC_BUTTON_A | CC_BUTTON_CONFIRM;
            FrameControl::WriteInput(ctx.isHost ? nav : GameInput{}, ctx.isHost ? GameInput{} : nav);
        } else WriteGameInputs(phase, 0, 0);
        runtime.previous = phase;
        runtime.previousIntro = intro;
        ++ctx.framesInPhase;
        return;
    }
    FrameControl::SetModeNormalSpeed();
    auto &metronome = Session::GetInstance().GetMetronome();
    int64_t offlineDue = 0;
    if (ctx.appMode != 0) {
        // オフラインではNetplaySession::Startを通らないため、初回通常更新で始動。
        // 起動中の高速化を終えた後、処理込みの絶対締切で60Hzを刻む。
        if (!metronome.IsRunning()) metronome.Start();
        const bool finalGate = OfflinePacing::Mode() != OfflinePacing::Variant::Legacy;
        offlineDue = metronome.WaitForNextTick(false, finalGate ?
            60 * cccaster::core::timer::FrameTiming::ReleasePreparationUs : 0);
        OfflinePacing::Waited(mem.WorldTimer(), offlineDue);
    }
    if (ctx.appMode != 0) {
        const bool configuring = cccaster::domain::ui::StateUiLogic::IsMappingWindowOpen();
        if (ctx.appMode == 1) {
            const auto beforeWorld = mem.WorldTimer();
            const auto sample = phase == GamePhase::InGame ? mem.ReadTrainingFrame() : TrainingFrameSample{};
            const auto event = runtime.trainingState.Step(ctx.appMode, phase == GamePhase::InGame,
                configuring, cccaster::game_interface::DirectInputHook::GetTrainingControls(), sample,
                mem, cccaster::platform::RealMonotonicUs());
            if (event == TrainingStateEvent::Loaded) {
                runtime.advantage.Reset();
                runtime.frameBar.Reset();
            }
            if (event != TrainingStateEvent::None)
                DebugLog("[TrainingState] event=%d saved=%d beforeWT=%u afterWT=%u", int(event),
                    int(runtime.trainingState.HasState()), beforeWorld, mem.WorldTimer());
        }
        FrameControl::WriteInput(
            runtime.localInputGate.Apply(
                GameInput::Unpack(cccaster::game_interface::DirectInputHook::GetPlayer1Input()), configuring),
            runtime.secondInputGate.Apply(
                GameInput::Unpack(cccaster::game_interface::DirectInputHook::GetPlayer2Input()),
                configuring));
        if (phase == GamePhase::CharaSelect) cccaster::diagnostics::startup::InputReady();
        runtime.previous = phase;
        runtime.previousIntro = intro;
        OfflinePacing::Prepared();
        if (OfflinePacing::Mode() != OfflinePacing::Variant::Legacy) {
            // 入力準備・Present復帰・ゲームのCS解放後に同じ絶対締切で解放する。
            // 準備にかかった時間を1フレームの周期へ追加しない。
            cccaster::core::timer::FrameTiming::releaseDueTicks = offlineDue;
            cccaster::core::timer::FrameTiming::releaseFrame = mem.WorldTimer();
            if (cccaster::diagnostics::UpdateCadence::Enabled())
                cccaster::diagnostics::UpdateCadence::Get().Arm(mem.WorldTimer(),
                    phase == GamePhase::InGame && intro == 0);
        }
        return;
    }
    auto &state = Session::GetMutableState();
    state.needKeepalive.store(true, std::memory_order_release);
    // 独立選択中も接続成立を通知し、選択時間を起動タイムアウトに含めない。
    if (state.isSynced.load(std::memory_order_acquire) && !runtime.syncedReported) {
        runtime.syncedReported = true;
        cccaster::public_api::IpcManager::UpdateOrReadState(
            [](cccaster::public_api::SharedState &s) { s.syncCompleted = true; });
    }
    if (phase == GamePhase::Rematch && mem.HasIndependentRetry()) {
        if (runtime.previous != phase) {
            state.retryPreviousFrame = state.consumedFrame.load();
            if (!runtime.sequence.Begin(0)) {
                Fail(Error::SyncTimeout, "retry epoch exhausted"); return;
            }
            mem.BeginIndependentRetry();
            runtime.snapshotReady = runtime.haveWorld = false;
            runtime.retryInputGate = {};
            runtime.retryTick = 0;
            const auto base = runtime.sequence.Base();
            {
                std::lock_guard lock(state.retryMutex);
                state.localRetry = {base, 0, 0};
            }
            state.phaseBaseFrame = base;
            state.localPhaseKind = static_cast<uint8_t>(phase);
            state.localPhaseReady = true;
            state.appliedFrame = state.consumedFrame = base;
            state.localPhaseToken = (uint64_t(base) << 32) | static_cast<uint8_t>(phase);
            timeline.Begin(base, runtime.sequence.Next(), phase, ctx.isHost);
            DebugLog("[RetryMenu] INDEPENDENT epoch=%u previous=%u", base, state.retryPreviousFrame.load());
        }
        if (state.protocolError || !state.isPeerAlive) {
            Fail(Error::SyncTimeout, "retry connection lost or incompatible peer"); return;
        }
        const auto frame = runtime.sequence.Next();
        if (!runtime.sequence.CanAdvance() || timeline.HasOverflowed()) {
            Fail(Error::SyncTimeout, "retry input range exhausted"); return;
        }
        if (!Wait("local retry clock", 3000000, [&] { return timeline.HasCaptured(frame); },
                  0, timeline.NextDeadlineTicks())) return;
        uint32_t input = 0;
        if (!MatchInputBuffer::GetInstance().TryGetLocalInput(frame, input)) {
            Fail(Error::SyncTimeout, "local retry input missing"); return;
        }
        RetrySelection local, peer;
        {
            std::lock_guard lock(state.retryMutex);
            local = state.localRetry;
            peer = state.peerRetry;
        }
        if (peer.epoch != local.epoch) peer = {};
        const int choice = mem.ReadRetryChoice();
        if (!local.choice && choice >= 0 && choice <= 1) {
            local.choice = uint32_t(choice + 1);
            DebugLog("[RetryMenu] LOCAL epoch=%u frame=%u choice=%d", local.epoch, frame, choice);
        }
        local.ack = peer.choice;
        {
            std::lock_guard lock(state.retryMutex);
            state.localRetry = local;
        }
        if (local.CanRelease(peer)) {
            const int result = local.Result(peer);
            if (runtime.broadcasting) {
                cccaster::spectator::Record record;
                record.Set(cccaster::spectator::Retry, frame, uint32_t(result));
                cccaster::spectator::Transport::Get().Publish(record);
            }
            mem.SetRetryTarget(result);
            timeline.Pause();
            runtime.fastRematchTransition = true;
            runtime.rematchTransitionStarted = cccaster::platform::RealMonotonicUs();
            FrameControl::SetModeHighSpeedSkip();
            FrameControl::WriteInput({}, {});
            DebugLog("[RetryMenu] RESOLVED epoch=%u frame=%u local=%d peer=%d target=%d ack=%u/%u",
                     local.epoch, frame, int(local.choice)-1, int(peer.choice)-1, result, local.ack, peer.ack);
        } else {
            auto own = runtime.retryInputGate.Apply(GameInput::Unpack(input));
            if (local.choice || local.Result(peer) >= 0) own = {};
            const auto due = timeline.CapturedDeadlineTicks(frame) +
                             60 * cccaster::core::timer::FrameTiming::SimulationGuardUs;
            cccaster::core::timer::FrameTiming::presentDueTicks = due +
                             60 * cccaster::core::timer::FrameTiming::PresentBudgetUs();
            if (!Wait("local retry deadline", 3000000,
                      [&] { return cccaster::core::timer::WasapiClock::GetTimeTicks() >= due; }, due)) return;
            FrameControl::WriteInput(ctx.isHost ? own : GameInput{}, ctx.isHost ? GameInput{} : own);
            if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled()) {
                const auto now = cccaster::platform::RealMonotonicUs();
                DebugLog("[RetryPace] f=%u WT=%u interval=%lld input=%u choice=%u peer=%u",
                         frame, world, runtime.retryTick ? now-runtime.retryTick : 0,
                         own.Pack(), local.choice, peer.choice);
                runtime.retryTick = now;
            }
            runtime.sequence.Commit(frame);
            timeline.SetConsumed(frame);
            state.appliedFrame = frame;
        }
        runtime.previous = phase;
        runtime.previousIntro = intro;
        ++ctx.framesInPhase;
        return;
    }
    if (phase == GamePhase::CharaSelect && mem.HasIndependentSelect()) {
        auto &buf = MatchInputBuffer::GetInstance();
        if (runtime.previous != phase || !runtime.sequence.Base()) {
            if (!runtime.sequence.Begin(0) || !mem.BeginIndependentSelect(ctx.isHost)) {
                Fail(Error::SyncTimeout, "independent character select hook unavailable");
                return;
            }
            runtime.snapshotReady = false;
            runtime.haveWorld = false;
            runtime.selectionReleased = false;
            runtime.spectatorSelectionSent = false;
            runtime.selectionTick = 0;
            runtime.selectionStarted = cccaster::platform::RealMonotonicUs();
            const auto base = runtime.sequence.Base();
            {
                std::lock_guard lock(state.selectionMutex);
                state.localSelection = {};
                state.localSelection.epoch = base;
                state.localSelection.revision = 1;
                state.localSelection.delay = SettingsCommands::delay;
                state.localSelection.rollback = SettingsCommands::rollback;
            }
            state.phaseBaseFrame = base;
            state.localPhaseKind = static_cast<uint8_t>(phase);
            state.localPhaseReady = true;
            state.appliedFrame = base;
            state.consumedFrame = base;
            state.localPhaseToken = (uint64_t(base) << 32) | static_cast<uint8_t>(phase);
            timeline.Begin(base, runtime.sequence.Next(), phase, ctx.isHost);
            DebugLog("[Select] INDEPENDENT epoch=%u", base);
        }
        if (state.protocolError || (state.isSynced && !state.isPeerAlive) ||
            (!state.isSynced && cccaster::platform::RealMonotonicUs() - runtime.selectionStarted > 30000000)) {
            Fail(Error::SyncTimeout, "character select connection lost or incompatible peer");
            return;
        }
        const auto frame = runtime.sequence.Next();
        if (!runtime.sequence.CanAdvance() || timeline.HasOverflowed()) {
            Fail(Error::SyncTimeout, "character select input range exhausted");
            return;
        }
        if (!Wait("local selection clock", 3000000, [&] { return timeline.HasCaptured(frame); },
                  0, timeline.NextDeadlineTicks())) return;
        uint32_t localInput = 0;
        if (!buf.TryGetLocalInput(frame, localInput)) {
            Fail(Error::SyncTimeout, "local selection input missing"); return;
        }
        SelectionState local, peer;
        {
            std::lock_guard lock(state.selectionMutex);
            local = state.localSelection;
            peer = state.peerSelection;
        }
        if (peer.epoch != local.epoch) peer = {};
        if (!mem.ReadLocalSelection(ctx.isHost, local)) {
            Fail(Error::SyncTimeout, "invalid final character selection"); return;
        }
        // 設定操作だけを確実に受け渡す。ホストが順序を決め、最終値を両者へ公開する。
        const auto command = localInput & ~SettingsCommands::GameMask;
        if (command && !local.confirmed && !runtime.selectionReleased) {
            if (ctx.isHost) SettingsCommands::Apply(command, true);
            else { local.command = command; ++local.commandSerial; }
            ++local.revision;
        } else if (command) {
            uint32_t expected = command;
            SettingsCommands::pending.compare_exchange_strong(expected, 0);
            SettingsCommands::notice = 3;
        }
        if (ctx.isHost) {
            if (peer.commandSerial > local.commandAck && !runtime.selectionReleased) {
                SettingsCommands::Apply(peer.command, false);
                local.commandAck = peer.commandSerial;
                ++local.revision;
            }
            local.delay = SettingsCommands::delay;
            local.rollback = SettingsCommands::rollback;
        } else if (peer.epoch) {
            if (local.commandSerial && peer.commandAck == local.commandSerial)
                SettingsCommands::Apply(local.command, true);
            SettingsCommands::delay = local.delay = peer.delay;
            SettingsCommands::rollback = local.rollback = peer.rollback;
        }
        local.ack = peer.revision;
        const auto remoteInput = mem.DriveRemoteSelection(ctx.isHost, peer, frame);
        const bool settingsReady = ctx.isHost ? !SettingsCommands::pending.load() :
            (!local.commandSerial || peer.commandAck == local.commandSerial);
        const auto &hostState = ctx.isHost ? local : peer;
        if (local.PeerHasFinal(peer) && hostState.stageConfirmed && settingsReady && state.isSynced)
            runtime.selectionReleased = true;
        if (runtime.broadcasting && runtime.selectionReleased && !runtime.spectatorSelectionSent) {
            // 選択確定を先に送る。観戦端末はロードを先行し、両者のイントロ合流通知を待つ。
            cccaster::spectator::StartData selected;
            selected.p1 = local; selected.p2 = peer;
            const auto rules = mem.SpectatorRules();
            selected.roundsToWin = rules[0]; selected.damageLevel = rules[1]; selected.timerSpeed = rules[2];
            const auto score = runtime.score.Snapshot();
            selected.score = {score.p1Wins, score.p2Wins, score.unresolved, uint32_t(score.revision)};
            { std::lock_guard lock(state.playerNameMutex);
              std::memcpy(selected.names[0], state.localPlayerName, 32); std::memcpy(selected.names[1], state.peerPlayerName, 32); }
            cccaster::spectator::Record record; record.Set(cccaster::spectator::Selection, frame, selected);
            cccaster::spectator::Transport::Get().Publish(record);
            runtime.spectatorSelectionSent = true;
            DebugLog("[Spectator] SELECT READY frame=%u qpc=%lld", frame, cccaster::platform::RealMonotonicUs());
        }
        mem.SetSelectionRelease(runtime.selectionReleased, hostState.stage);
        {
            std::lock_guard lock(state.selectionMutex);
            state.localSelection = local;
        }
        // キャラ確定後は戻る操作を閉じ、ステージ操作はホストだけが所有する。
        auto own = GameInput::Unpack(localInput & SettingsCommands::GameMask);
        if (local.confirmed) {
            own.buttons &= ~(CC_BUTTON_B | CC_BUTTON_CANCEL);
            if (!ctx.isHost || local.stageConfirmed) own = {};
        }
        const auto due = timeline.CapturedDeadlineTicks(frame) +
                         60 * cccaster::core::timer::FrameTiming::SimulationGuardUs;
        cccaster::core::timer::FrameTiming::presentDueTicks = due +
                         60 * cccaster::core::timer::FrameTiming::PresentBudgetUs();
        if (!Wait("local selection deadline", 3000000,
                  [&] { return cccaster::core::timer::WasapiClock::GetTimeTicks() >= due; }, due)) return;
        FrameControl::WriteInput(ctx.isHost ? own : remoteInput, ctx.isHost ? remoteInput : own);
        if (std::getenv("CCCASTER_PACE_TRACE")) {
            const auto now = cccaster::platform::RealMonotonicUs();
            DebugLog("[SelectPace] f=%u WT=%u interval=%lld local=%u peer=%u", frame, world,
                     runtime.selectionTick ? now - runtime.selectionTick : 0, own.Pack(), peer.revision);
            runtime.selectionTick = now;
        }
        if (cccaster::testing::IsScriptedInputEnabled() && frame % 60 == 0)
            DebugLog("[Select] TICK f=%u WT=%u peer=%u final=%u/%u stage=%u release=%d",
                     frame, world, peer.revision, local.confirmed, peer.confirmed,
                     hostState.stageConfirmed, runtime.selectionReleased);
        runtime.sequence.Commit(frame);
        timeline.SetConsumed(frame);
        state.appliedFrame = frame; // ローカル実更新の計測用。相手消費ACKとは別。
        runtime.previous = phase;
        runtime.previousIntro = intro;
        ++ctx.framesInPhase;
        cccaster::diagnostics::startup::InputReady();
        return;
    }
    if (!Wait("handshake", 30000000, [&] { return state.isSynced.load(std::memory_order_acquire); }))
        return;
    if (!runtime.syncedReported) {
        runtime.syncedReported = true;
        cccaster::public_api::IpcManager::UpdateOrReadState(
            [](cccaster::public_api::SharedState &s) { s.syncCompleted = true; });
    }

    const bool inputPhase =
        phase == GamePhase::CharaSelect || phase == GamePhase::InGame || phase == GamePhase::Rematch;
    if (!inputPhase) {
        // ロード中は入力消費しない。速い側は次の世代の入口で待つ。
        FrameControl::WriteInput({}, {});
        runtime.previous = phase;
        runtime.previousIntro = intro;
        runtime.haveWorld = false;
        ++ctx.framesInPhase;
        return;
    }
    const bool newRound =
        phase == GamePhase::InGame && runtime.previous == phase && intro == 2 && runtime.previousIntro != 2;
    const bool boundary = phase != runtime.previous || runtime.sequence.Base() == 0 || newRound;
    auto &buf = MatchInputBuffer::GetInstance();
    if (boundary) {
        timeline.Pause();
        // 相手が旧世代の末尾を消費するまで旧履歴を再送し続ける。
        // 先に新世代へ進むと、末尾パケットを失った相手が永久に取り残される。
        const auto lastConsumed = state.consumedFrame.load(std::memory_order_acquire);
        if (lastConsumed && !Wait("previous epoch drain", 3000000, [&] {
                if (state.peerConsumedFrame.load(std::memory_order_acquire) >= lastConsumed)
                    return true;
                // 再戦へ到達済みの相手は旧対戦の入力をもう消費しない。
                // 終了演出の末尾が1F異なる場合、消費ACKだけでは双方が待ち合う。
                // 直後の再戦世代への到達通知のみを終了証明として認める。
                const auto peerToken = state.peerPhaseToken.load(std::memory_order_acquire);
                if (phase == GamePhase::Rematch &&
                    runtime.sequence.PeerEnteredNextPhase(peerToken, static_cast<uint8_t>(phase))) {
                    DebugLog("[Rematch] PEER CLOSED previous=%u localEnd=%u peerEnd=%u next=%u",
                             runtime.sequence.Base(), lastConsumed, state.peerConsumedFrame.load(),
                             static_cast<uint32_t>(peerToken >> 32));
                    return true;
                }
                return false;
            }))
            return;
        cccaster::core::sync::SettingsCommands::Boundary();
        ctx.delay = cccaster::core::sync::SettingsCommands::delay.load();
        ctx.maxRollback = cccaster::core::sync::SettingsCommands::rollback.load();
        buf.SetSyncParams(ctx.delay, ctx.maxRollback);
        cccaster::domain::ui::StateUiLogic::SetDelay(ctx.delay);
        cccaster::domain::ui::StateUiLogic::SetRollback(ctx.maxRollback);
        DebugLog("[Settings] ACTIVE D=%d R=%d", int(ctx.delay), int(ctx.maxRollback));
        const int delay = int(ctx.delay);
        if (!cccaster::public_api::NetplaySettings::IsValid(ctx.delay, ctx.maxRollback)) {
            Fail(Error::SyncTimeout, "D+R exceeds 8");
            return;
        }
        if (delay < 0 || !runtime.sequence.Begin(static_cast<uint32_t>(delay))) {
            Fail(Error::SyncTimeout, "unsupported delay (D+R must be 0..8) or epoch exhausted");
            return;
        }
        runtime.history.Reset(runtime.sequence.Next(), ctx.maxRollback);
        runtime.postRoundDelay.Reset();
        runtime.snapshotReady = phase == GamePhase::InGame && ctx.maxRollback > 0 && mem.SupportsSnapshots();
        if (runtime.snapshotReady && !mem.SnapshotSize()) {
            Fail(Error::SyncTimeout, "rollback hooks unavailable");
            return;
        }
        if (runtime.snapshotReady)
            runtime.snapshots.Reset(mem.SnapshotSize());
        if (phase == GamePhase::InGame && !mem.PrepareBattleAudio()) {
            Fail(Error::SyncTimeout, "sound preparation restoration failed");
            return;
        }
        ctx.phaseBaseWorldTimer = world;
        runtime.haveWorld = false;
        if (ctx.isHost) {
            cccaster::game_interface::RngState seed{};
            if (!mem.ReadRng(seed)) {
                Fail(Error::SyncTimeout, "RNG capture failed");
                return;
            }
            std::lock_guard<std::mutex> lock(state.seedMutex);
            state.localSeed = seed;
            state.localSeedEpoch = runtime.sequence.Base();
        }
        state.appliedFrame.store(runtime.sequence.Base(), std::memory_order_release);
        state.localPhaseReady.store(false, std::memory_order_release);
        // 全消去せず、先に到達した相手からの今世代入力を残す。
        for (uint32_t i = 0; i < runtime.sequence.Lookahead(); ++i)
            buf.WriteLocal(runtime.sequence.Next() + i, GameInput{}.Pack(), 0, false);
        state.localPhaseKind.store(static_cast<uint8_t>(phase), std::memory_order_relaxed);
        state.phaseBaseFrame.store(runtime.sequence.Base(), std::memory_order_release);
        state.localPhaseReady.store(true, std::memory_order_release);
        state.localPhaseToken.store((uint64_t(runtime.sequence.Base()) << 32) | static_cast<uint8_t>(phase),
                                    std::memory_order_release);
        DebugLog("[InputGate] EPOCH base=%u phase=%u WT=%u lookahead=%u", runtime.sequence.Base(),
                 static_cast<unsigned>(phase), world, runtime.sequence.Lookahead());
    }
    if (runtime.haveWorld && world == runtime.lastWorld)
        return;
    if (runtime.haveWorld && world - runtime.lastWorld != 1) {
        Fail(Error::SyncTimeout, "missed game update");
        return;
    }
    if (!runtime.sequence.CanAdvance()) {
        Fail(Error::SyncTimeout, "epoch frame range exhausted");
        return;
    }
    buf.SetGameReadFrame(runtime.sequence.Next());
    if (boundary && !Wait("epoch barrier", 30000000, [&] {
            const auto peerToken = state.peerPhaseToken.load(std::memory_order_acquire);
            const uint32_t peerBase = uint32_t(peerToken >> 32);
            if (peerBase > runtime.sequence.Base() ||
                (peerBase == runtime.sequence.Base() && uint8_t(peerToken) != static_cast<uint8_t>(phase))) {
                state.protocolError.store(true, std::memory_order_release);
                return false;
            }
            return peerBase == runtime.sequence.Base();
        }))
        return;

    if (boundary && !ctx.isHost) {
        cccaster::game_interface::RngState seed{};
        if (!Wait("epoch RNG", 30000000, [&] {
                std::lock_guard<std::mutex> lock(state.seedMutex);
                if (state.peerSeedEpoch != runtime.sequence.Base())
                    return false;
                seed = state.peerSeed;
                return true;
            }))
            return;
        if (!mem.WriteRng(seed)) {
            Fail(Error::SyncTimeout, "RNG restore failed");
            return;
        }
    }

    if (boundary) {
        if (phase == GamePhase::InGame) mem.AlignIntroRng();
        if (runtime.broadcasting && phase == GamePhase::InGame) {
            DebugLog("[Spectator] BOTH INTRO frame=%u qpc=%lld", runtime.sequence.Next(), cccaster::platform::RealMonotonicUs());
            cccaster::spectator::Record record;
            if (!newRound) {
                cccaster::spectator::StartData start;
                mem.ReadRng(start.rng);
                { std::lock_guard lock(state.selectionMutex); start.p1 = state.localSelection; start.p2 = state.peerSelection; }
                { std::lock_guard lock(state.playerNameMutex);
                  std::memcpy(start.names[0], state.localPlayerName, 32); std::memcpy(start.names[1], state.peerPlayerName, 32); }
                const auto score = runtime.score.Snapshot();
                start.score = {score.p1Wins, score.p2Wins, score.unresolved, uint32_t(score.revision)};
                const auto rules = mem.SpectatorRules();
                start.roundsToWin = rules[0]; start.damageLevel = rules[1]; start.timerSpeed = rules[2];
                record.Set(cccaster::spectator::Start, runtime.sequence.Next(), start);
            } else {
                cccaster::game_interface::RngState rng{}; mem.ReadRng(rng);
                record.Set(cccaster::spectator::Epoch, runtime.sequence.Next(), rng);
            }
            cccaster::spectator::Transport::Get().Publish(record);
            runtime.spectatorNext = runtime.sequence.Next();
        }
        int64_t firstTicks = 0;
        if (phase == GamePhase::InGame) {
            cccaster::core::sync::EpochStartGate gate;
            gate.Begin(runtime.sequence.Base());
            bool expired = false;
            if (!Wait("epoch future start", 30000000, [&] {
                    std::lock_guard lock(state.epochStartMutex);
                    const auto result = gate.Update(ctx.isHost, state.peerEpochStart,
                        state.peerEpochStartLocalTicks, cccaster::core::timer::WasapiClock::GetTimeTicks(),
                        std::clamp<int64_t>(state.lastRttUs.load(), 0, 1000000)*60);
                    state.localEpochStart = gate.local;
                    expired = result == cccaster::core::sync::EpochStartGate::Expired;
                    return expired || result == cccaster::core::sync::EpochStartGate::Armed;
                })) return;
            if (expired || gate.dueTicks <= cccaster::core::timer::WasapiClock::GetTimeTicks()) {
                Fail(Error::SyncTimeout, "epoch start agreement arrived late");
                return;
            }
            firstTicks = gate.dueTicks;
            DebugLog("[EpochStart] epoch=%u serial=%u hostUs=%lld localUs=%lld firstCapture=%u role=%d",
                gate.local.epoch, gate.local.serial, gate.local.hostTicks/60, gate.dueTicks/60,
                runtime.sequence.Capture(), int(ctx.isHost));
        }
        timeline.Begin(runtime.sequence.Base(), runtime.sequence.Capture(), phase, ctx.isHost, firstTicks);
    } else if (!timeline.IsActive())
        timeline.Resume();
    const auto captureHint = timeline.NextDeadlineTicks();
    if (!Wait(
            "metronome input", 3000000,
            [&] { return timeline.HasCaptured(runtime.sequence.Capture()) || timeline.HasOverflowed(); }, 0,
            captureHint))
        return;
    if (timeline.HasOverflowed()) {
        Fail(Error::SyncTimeout, "input clock backlog exhausted");
        return;
    }
    // ゲームの処理遅延分も入力枠は捨てず、描画待機を止めて順番に追いつく。
    const auto sampledFrame = timeline.SampledFrame();
    if (sampledFrame > runtime.sequence.Capture()) {
        FrameControl::SetModeHighSpeedSkip();
        static const bool catchupTrace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
        if (catchupTrace)
            DebugLog("[InputCatchup] f=%u capture=%u sampled=%u lateTicks=%lld",
                runtime.sequence.Next(), runtime.sequence.Capture(), sampledFrame,
                cccaster::core::timer::WasapiClock::GetTimeTicks() -
                    timeline.CapturedDeadlineTicks(runtime.sequence.Capture()));
    }
    uint32_t p1 = 0, p2 = 0;
    // 起動・キャラロード中のデバッガ接続を避け、戦闘世代360F後に一度だけ通知。
    if (phase == GamePhase::InGame && runtime.sequence.Next() >= runtime.sequence.Base() + 360)
        cccaster::diagnostics::NotifySpikeDebugReady();
    uint32_t localValue = 0, remoteValue = 0;
    if (!buf.TryGetLocalInput(runtime.postRoundDelay.Source(runtime.sequence.Next()), localValue)) {
        Fail(Error::SyncTimeout, "local input missing");
        return;
    }
    bool remoteReady = readRemote(runtime.sequence.Next(), remoteValue);
    const bool allowPrediction = runtime.snapshotReady && mem.CanRollback() && !runtime.postRoundDelay.first;
    const bool mayPredict = allowPrediction && runtime.history.CanPredict();
    if (!remoteReady && mayPredict) {
        remoteValue = runtime.history.Prediction();
        if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
            DebugLog("[Rollback] PREDICT frame=%u confirmed=%u", runtime.sequence.Next(),
                     runtime.history.Confirmed());
    } else if (!remoteReady || (runtime.snapshotReady && !runtime.history.CanPredict())) {
        if (!Wait("remote input", 3000000, [&] {
                if (runtime.snapshotReady)
                    return runtime.history.ReadyToResume(readRemote, allowPrediction);
                return readRemote(runtime.sequence.Next(), remoteValue);
            }))
            return;
        const auto mismatch = runtime.snapshotReady ? runtime.history.Reconcile(readRemote) : 0;
        if (mismatch) {
            const bool replayProbe = cccaster::diagnostics::SpinProbe::Enabled();
            if (replayProbe) cccaster::diagnostics::DeferredNumericLog::Prepare();
            runtime.replayBeginTicks = replayProbe ? cccaster::platform::RealMonotonicTicks() : 0;
            runtime.replaySaveTicks = runtime.replayPrepareTicks = 0;
            runtime.replaySaved = runtime.replaySkipped = 0;
            runtime.replayFrom = mismatch;
            const unsigned targetIntro = mem.IntroState();
            const auto restoreStart = cccaster::platform::RealMonotonicUs();
            if (!mem.BeginReplay(mismatch, runtime.sequence.Next()) ||
                !runtime.snapshots.Load(mismatch, mem)) {
                Fail(Error::SyncTimeout, "rollback restore failed");
                return;
            }
            if (std::getenv("CCCASTER_PACE_TRACE"))
                DebugLog("[RestoreStage] f=%u restore=%lld", runtime.sequence.Next(),
                         cccaster::platform::RealMonotonicUs() - restoreStart);
            runtime.replayRestoreEnd = replayProbe ? cccaster::platform::RealMonotonicTicks() : 0;
            runtime.replayFrame = mismatch;
            runtime.replayTarget = runtime.sequence.Next();
            runtime.replayStarted = cccaster::platform::RealMonotonicUs();
            cccaster::domain::ui::StateUiLogic::RecordRollback(runtime.replayTarget - mismatch);
            if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
                cccaster::diagnostics::DeferredNumericLog::Log("[Rollback] BEGIN frame=%u target=%u depth=%u forced=0 intro=%u targetIntro=%u", mismatch,
                         runtime.replayTarget, runtime.replayTarget - mismatch, unsigned(mem.IntroState()), targetIntro);
            // 1回だけ入り直して最初の再計算入力を設定。ゲーム更新そのものはPresentから戻って行う。
            Step();
            return;
        }
    }
    // 待機中に最古入力が確定した場合、最新入力が未着でも空いた枠を使える。
    // Reconcileの不一致は上で復元へ戻すため、訂正を取り落とさない。
    if (!readRemote(runtime.sequence.Next(), remoteValue)) {
        if (!allowPrediction || !runtime.history.CanPredict()) {
            Fail(Error::SyncTimeout, "prediction capacity unavailable");
            return;
        }
        remoteValue = runtime.history.Prediction();
    }
    p1 = ctx.isHost ? localValue : remoteValue;
    p2 = ctx.isHost ? remoteValue : localValue;
    static const bool stageTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    const auto saveStart = stageTrace ? cccaster::platform::RealMonotonicUs() : 0;
    if (runtime.snapshotReady && (mem.CanRollback() || cccaster::diagnostics::UpdateCadence::Evidence()) &&
        !runtime.snapshots.Save(runtime.sequence.Next(), mem, localValue, remoteValue)) {
        Fail(Error::SyncTimeout, "snapshot save failed");
        return;
    }
    if (stageTrace)
        DebugLog("[SnapshotStage] f=%u save=%lld", runtime.sequence.Next(),
                 cccaster::platform::RealMonotonicUs() - saveStart);
    if (runtime.snapshotReady &&
        !runtime.history.Record(runtime.sequence.Next(), ctx.isHost ? p1 : p2, ctx.isHost ? p2 : p1)) {
        Fail(Error::SyncTimeout, "prediction history sequence");
        return;
    }
    if (phase == GamePhase::CharaSelect) {
        using Commands = cccaster::core::sync::SettingsCommands;
        const bool first = Commands::Apply(p1, ctx.isHost);
        const bool second = Commands::Apply(p2, !ctx.isHost);
        if (first || second)
            DebugLog("[Settings] AGREED frame=%u D=%d R=%d", runtime.sequence.Next(), Commands::delay.load(),
                     Commands::rollback.load());
    }
    p1 &= cccaster::core::sync::SettingsCommands::GameMask;
    p2 &= cccaster::core::sync::SettingsCommands::GameMask;
    if (runtime.broadcasting && !runtime.snapshotReady && phase == GamePhase::InGame) {
        runtime.spectatorPending.Set(cccaster::spectator::Input, runtime.sequence.Next(), std::array<uint32_t, 2>{p1, p2});
        runtime.spectatorHasPending = true; // 次Stepの準備区間で公開。締切後にコピーしない。
    }
    // 採取の予定時刻を基準に更新開始を揃える。相手待ちや保存の所要時間を次の周期に足さない。
    // 3msは入力公開・保存の余裕。ロールアップ経路はここを通らず即時に再計算する。
    const auto captureDue = timeline.CapturedDeadlineTicks(runtime.sequence.Capture());
    const auto simulationDue =
        captureDue ? captureDue + 60 * cccaster::core::timer::FrameTiming::SimulationGuardUs : 0;
    const auto preparationDue = simulationDue ? simulationDue -
        60 * cccaster::core::timer::FrameTiming::ReleasePreparationUs : 0;
    cccaster::core::timer::FrameTiming::presentDueTicks =
        captureDue ? simulationDue + 60 * cccaster::core::timer::FrameTiming::PresentBudgetUs() : 0;
    static const bool paceTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    const auto readyAudio = paceTrace ? cccaster::core::timer::WasapiClock::GetTimeUs() : 0;
    using Probe = cccaster::diagnostics::SpinProbe;
    const bool probe = Probe::Enabled() && simulationDue && runtime.snapshotReady && mem.CanRollback();
    // 締切前の保存済み数値だけを整形。採取・スピン内にログを挟まない。
    static const bool clockFollowTrace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
    if (probe || clockFollowTrace) timeline.TraceCapturedPhase(runtime.sequence.Capture());
    if (probe) Probe::Start(runtime.sequence.Next(), runtime.snapshotReady && mem.CanRollback(),
                            preparationDue, cccaster::core::timer::WasapiClock::GetTimeTicks());
    if (!probe)
        cccaster::diagnostics::DeferredNumericLog::Prepare();
    if (simulationDue &&
        !Wait(
            "simulation deadline", 3000000,
            [&] { return cccaster::core::timer::WasapiClock::GetTimeTicks() >= preparationDue; },
            preparationDue))
        return;
    if (paceTrace && !probe) {
        const auto q0 = cccaster::platform::RealMonotonicUs();
        const auto audio = cccaster::core::timer::WasapiClock::GetTimeUs();
        const auto q1 = cccaster::platform::RealMonotonicUs();
        cccaster::diagnostics::DeferredNumericLog::Log("[Pace] f=%u due=%lld ready=%lld audio=%lld qpc=%lld read=%lld work=%lld play=%d",
                 runtime.sequence.Next(), simulationDue / 60, readyAudio, audio, (q0 + q1) / 2, q1 - q0,
                 cccaster::core::timer::FrameTiming::workUs, runtime.snapshotReady && mem.CanRollback());
    }
    mem.BeginSimulation(runtime.sequence.Next());
    if (probe) Probe::sample.begin = Probe::Now();
    WriteGameInputs(phase, p1, p2);
    if (phase == GamePhase::CharaSelect) cccaster::diagnostics::startup::InputReady();
    if (probe) Probe::sample.input = Probe::Now();
    TraceFrame(runtime.sequence.Next(), GameInput::Unpack(p1), GameInput::Unpack(p2));
    if (probe) Probe::sample.trace = Probe::Now();
    if (!runtime.sequence.Commit(runtime.sequence.Next())) {
        Fail(Error::SyncTimeout, "duplicate consumption");
        return;
    }
    if (!runtime.snapshotReady) {
        state.consumedFrame.store(runtime.sequence.Next() - 1, std::memory_order_release);
        if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsInputTraceEnabled())
            cccaster::diagnostics::DeferredNumericLog::Log("[CONFIRMED] %u", runtime.sequence.Next() - 1);
    }
    timeline.SetConsumed(runtime.sequence.Next() - 1);
    state.appliedFrame.store(runtime.sequence.Next() - 1, std::memory_order_release);
    if (probe) Probe::sample.commit = Probe::Now();
    runtime.lastWorld = world;
    runtime.haveWorld = true;
    runtime.previous = phase;
    runtime.previousIntro = intro;
    ++ctx.framesInPhase;
    if (probe) Probe::sample.step = Probe::Now();
    cccaster::core::timer::FrameTiming::releaseDueTicks = simulationDue;
    cccaster::core::timer::FrameTiming::releaseFrame = runtime.sequence.Next() - 1;
    if (cccaster::diagnostics::UpdateCadence::Enabled())
        cccaster::diagnostics::UpdateCadence::Get().Arm(runtime.sequence.Next() - 1,
            phase == GamePhase::InGame && intro == 0);
}
bool SceneRunner::IsReplaying() {
    return runtime.replayFrame && runtime.replayFrame < runtime.replayTarget;
}
bool SceneRunner::IsReady() {
    return runtime.ready.load(std::memory_order_acquire);
}
uint8_t SceneRunner::AppMode() {
    return IsReady() && runtime.context ? runtime.context->appMode : uint8_t{255};
}
SessionScoreSnapshot SceneRunner::Score() {
    if (AppMode() == 2) {
        const auto &s = runtime.spectator.score;
        return {s.p1, s.p2, s.unresolved, s.revision, 0, true};
    }
    return runtime.score.Snapshot();
}
SceneRunner::PlayerNamesSnapshot SceneRunner::PlayerNames() {
    PlayerNamesSnapshot result{};
    if (!IsReady() || !runtime.context)
        return result;
    if (AppMode() == 2) {
        std::memcpy(result.p1.data(), runtime.spectator.match.names[0], 32);
        std::memcpy(result.p2.data(), runtime.spectator.match.names[1], 32);
        return result;
    }
    auto &shared = Session::GetMutableState();
    std::lock_guard lock(shared.playerNameMutex);
    const char *p1 = runtime.context->isHost ? shared.localPlayerName : shared.peerPlayerName;
    const char *p2 = runtime.context->isHost ? shared.peerPlayerName : shared.localPlayerName;
    cccaster::public_api::NormalizePlayerName(result.p1.data(), result.p1.size(), p1, "PLAYER 1");
    cccaster::public_api::NormalizePlayerName(result.p2.data(), result.p2.size(), p2, "PLAYER 2");
    return result;
}
FrameAdvantageResult SceneRunner::FrameAdvantage() {
    return IsReady() ? runtime.advantage.Result() : FrameAdvantageResult{};
}

const FrameBarHistory &SceneRunner::FrameBar() {
    return runtime.frameBar;
}
TrainingStateEvent SceneRunner::TrainingStateNotice() {
    return IsReady() && AppMode() == 1 ? runtime.trainingState.Notice(cccaster::platform::RealMonotonicUs())
                                     : TrainingStateEvent::None;
}
bool SceneRunner::HasTrainingState() {
    return IsReady() && AppMode() == 1 && runtime.trainingState.HasState();
}
SceneRunner::SpectatorInfo SceneRunner::SpectatorStatus() {
    auto &wire = cccaster::spectator::Transport::Get();
    return {runtime.spectator.Last(), wire.Latest(), wire.Viewers(), uint32_t(wire.State()), runtime.spectator.Catching()};
}
} // namespace cccaster::domain::session
