#include "core_dll/sync/InputTimeline.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/sync/FrameSequence.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/engine/SceneInputFilter.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include <cstdlib>
#include "core_dll/common/Platform.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
#include "core_dll/common/TimeScale.hpp"
#include "core_dll/common/InputTrace.hpp"
namespace cccaster::core::sync {
InputTimeline &InputTimeline::GetInstance() {
    static InputTimeline timeline;
    return timeline;
}
void InputTimeline::Reset() {
    std::lock_guard lock(mutex_);
    active_ = false;
    overflow_ = false;
    sampled_ = 0;
    consumed_ = 0;
    gate_ = {};
    base_ = next_ = lastValue_ = 0;
    captureTimes_ = {};
}
void InputTimeline::Begin(uint32_t base, uint32_t firstCapture, game_interface::GamePhase phase, bool host, int64_t firstTicks) {
    std::lock_guard lock(mutex_);
    phaseParts_ = rateParts_ = 0;
    modelRevision_ = 0; modelReady_ = false;
    follower_.Reset();
    phaseError_ = phaseShift_ = phaseTheta_ = phaseRtt_ = 0;
    base_ = base;
    next_ = firstCapture;
    phase_ = phase;
    host_ = host;
    lastValue_ = 0;
    sampled_ = firstCapture - 1;
    captureTimes_ = {};
    consumed_ = base;
    overflow_ = false;
    cadence_.ResetTicks(firstTicks ? firstTicks : timer::WasapiClock::GetTimeTicks());
    starting_ = firstTicks != 0;
    domain::scene::SceneInputFilter::Reset();
    active_.store(true, std::memory_order_release);
    PublishSchedule();
    netplay::NetplaySession::GetInstance().WakeInputClock();
}
void InputTimeline::Pause() {
    std::lock_guard lock(mutex_);
    active_ = false;
    PublishSchedule();
    netplay::NetplaySession::GetInstance().WakeInputClock();
}
void InputTimeline::Resume() {
    std::lock_guard lock(mutex_);
    if (!overflow_ && base_) {
        active_ = true;
        netplay::NetplaySession::GetInstance().WakeInputClock();
    }
}
void InputTimeline::PumpTicks(int64_t nowTicks, int64_t periodCorrectionParts) {
    const auto nowUs = nowTicks / 60; // 診断ログだけに使用。
    const auto scale = testing::TimeScale();
    static const bool stages = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    const auto entered = stages ? platform::RealMonotonicUs() : 0;
    std::unique_lock lock(mutex_);
    const auto locked = stages ? platform::RealMonotonicUs() : 0;
    int64_t pollUs = 0, publishUs = 0;
    const uint32_t firstSample = next_;
    const auto firstDue = cadence_.NextTicks() / 60;
    static const bool sendTrace = std::getenv("CCCASTER_INPUT_SEND_TRACE") != nullptr;
    struct CaptureSend { uint32_t frame; int64_t begin, end, published; };
    std::array<CaptureSend, 32> sendSamples;
    unsigned sendSampleCount = 0, sendSampleDropped = 0;

    if (!active_ || nowTicks < cadence_.NextTicks()) return;
    netplay::SharedSyncState::InputSchedule peer;
    {
        auto &shared = netplay::NetplaySession::GetMutableState();
        std::lock_guard scheduleLock(shared.scheduleMutex);
        peer = shared.peerSchedule;
    }
    const bool fresh = !host_ && phase_ != game_interface::GamePhase::CharaSelect &&
        peer.base == base_ && peer.frame > base_ && peer.stampTicks > 0 &&
        nowTicks >= peer.stampTicks && nowTicks-peer.stampTicks < 250000LL*60;
    using game_interface::GameInput;
    // 未消費枠と再送履歴を残す。ゲームが停止しても上書きしない。
    unsigned sampled = 0;
    while (nowTicks >= cadence_.NextTicks()) {
        if (next_ - consumed_.load(std::memory_order_acquire) >= MatchInputBuffer::RING_SIZE - 32 ||
            next_ >= base_ + FrameSequence::STRIDE - 1) {
            overflow_ = true;
            active_ = false;
            PublishSchedule();
            return;
        }
        starting_ = false;
        const bool missed = nowTicks - cadence_.NextTicks() >= timer::ClockFrame / scale;
        static const bool timingTrace = std::getenv("CCCASTER_FRAME_TIMING_TRACE") != nullptr;
        if (timingTrace) {
            static int64_t maximum = 0, total = 0;
            static unsigned count = 0;
            const auto late = std::max<int64_t>(0, nowUs - cadence_.NextUs());
            maximum = std::max(maximum, late);
            total += late;
            if (++count == 120) {
                domain::session::DebugLog("[CaptureTiming] count=120 meanLateUs=%lld maxLateUs=%lld",
                                          total / 120, maximum);
                count = 0;
                maximum = total = 0;
            }
        }
        uint32_t value = lastValue_;
        const auto captureBegin = sendTrace && !missed ? platform::RealMonotonicTicks() : 0;
        if (!missed) {
            const auto pollStart = stages ? platform::RealMonotonicUs() : 0;
            if (testing::IsScriptedInputEnabled()) {
                value = testing::ScriptedInput(next_ - base_, host_);
                // 連続ドローの再現専用。戦闘と決着演出を無操作で自然終了させる。
                static const bool drawIdle = std::getenv("CCCASTER_TEST_DRAW_IDLE") != nullptr;
                if (drawIdle && phase_ == game_interface::GamePhase::InGame) value = 0;
                // 自動試験専用: 登場演出をボタンで飛ばさず、自然終了経路も通す。
                static const bool fullIntro = std::getenv("CCCASTER_TEST_FULL_INTRO") != nullptr;
                if (fullIntro && phase_ == game_interface::GamePhase::InGame && next_ - base_ < 480)
                    value = GameInput{GameInput::Unpack(value).direction, 0}.Pack();
                if (phase_ == game_interface::GamePhase::Rematch) {
                    if (const char *scenario = std::getenv("CCCASTER_TEST_REMATCH")) {
                        const auto f = next_ - base_;
                        const bool chara = (scenario[0] == '1' && host_) || (scenario[0] == '2' && !host_);
                        value = 0;
                        if (chara && f == 60)
                            value = GameInput{2, 0}.Pack();
                        if ((chara && f == 64) || (scenario[0] == '0' && f == (host_ ? 60u : 300u)) ||
                            (scenario[0] == '3' && f == (host_ ? 2400u : 2460u)))
                            value = GameInput{0, CC_BUTTON_A}.Pack();
                        if (std::getenv("CCCASTER_TEST_NATIVE_RETRY")) {
                            // 元ゲームは最初の決定で結果メニューを開く。次の決定で項目を選ぶ。
                            value = 0;
                            if (chara) {
                                if (f == 60 || (f >= 144 && f % 24 == 0))
                                    value = GameInput{0, CC_BUTTON_A}.Pack();
                                if (f == 120) value = GameInput{2, 0}.Pack();
                            } else if ((scenario[0] == '0' && f >= (host_ ? 60u : 300u)) ||
                                       (scenario[0] == '3' && f >= (host_ ? 2400u : 2460u))) {
                                if (f % 24 == 12) value = GameInput{0, CC_BUTTON_A}.Pack();
                            }
                            if (scenario[0] == '3' && f == 60) value = GameInput{0, CC_BUTTON_A}.Pack();
                        }
                    }
                }
            } else {
                game_interface::DirectInputHook::Poll();
                value = game_interface::DirectInputHook::GetLocalPlayerInput(host_, true);
            }
            if (testing::IsScriptedInputEnabled() && phase_ == game_interface::GamePhase::CharaSelect) {
                if (std::getenv("CCCASTER_TEST_RANDOM_STAGE"))
                    value = GameInput{0, static_cast<uint16_t>((next_ - base_) % 24 == 18 ? CC_BUTTON_CONFIRM : 0)}.Pack();
                if (std::getenv("CCCASTER_TEST_SELECTION_IDLE") && !host_ && next_ - base_ < 420)
                    value = 0; // 自分の操作が相手の7秒無操作に引きずられない実機試験。
                const char *settingsTest = std::getenv("CCCASTER_TEST_SETTINGS");
                if (settingsTest && settingsTest[0] == '2')
                    value = 0; // UI確認用にキャラセレで待機。
            }
            lastValue_ = value;
            if (stages)
                pollUs += platform::RealMonotonicUs() - pollStart;
        }
        const auto captureEnd = sendTrace && !missed ? platform::RealMonotonicTicks() : 0;
        const auto raw = value;
        const bool mapping = domain::ui::StateUiLogic::IsMappingWindowOpen();
        value = gate_.Apply(GameInput::Unpack(value), mapping).Pack();
        if (phase_ != game_interface::GamePhase::Rematch)
            value = domain::scene::SceneInputFilter::Apply(phase_, value);
        if (testing::IsInputTraceEnabled()) {
            static uint32_t previousRaw = ~0u, previousValue = ~0u;
            static bool previousMapping = false;
            if (raw != previousRaw || value != previousValue || mapping != previousMapping) {
                domain::session::DebugLog("[INPUT] frame=%u raw=%u published=%u mapping=%d missed=%d", next_,
                                          raw, value, mapping, missed);
                previousRaw = raw;
                previousValue = value;
                previousMapping = mapping;
            }
        }
        if (phase_ == game_interface::GamePhase::CharaSelect) {
            // 明示的な実機疎通試験でのみ、両端から設定操作を送る。
            if (testing::IsScriptedInputEnabled() && std::getenv("CCCASTER_TEST_SETTINGS") &&
                std::getenv("CCCASTER_TEST_SETTINGS")[0] == '1') {
                if (host_ && next_ - base_ == 80)
                    SettingsCommands::Request(false, 3);
                if (!host_ && next_ - base_ == 110)
                    SettingsCommands::Request(true, 2);
                if (host_ && next_ - base_ == 140)
                    SettingsCommands::Request(false, 8);
            }
            value |= SettingsCommands::Capture();
        }
        if (sendTrace && !missed && phase_ == game_interface::GamePhase::InGame) {
            if (sendSampleCount < sendSamples.size())
                sendSamples[sendSampleCount++] = {next_, captureBegin, captureEnd, platform::RealMonotonicTicks()};
            else ++sendSampleDropped;
        }
        MatchInputBuffer::GetInstance().WriteLocal(next_, value, 0, false);
        captureTimes_[next_ % captureTimes_.size()] = {next_, cadence_.NextTicks(), phaseError_, phaseShift_, phaseTheta_, phaseRtt_,
            phaseParts_,rateParts_,modelRevision_,modelReady_};
        phaseShift_ = 0;
        sampled_.store(next_++, std::memory_order_release);
        if (stages)
            publishUs = platform::RealMonotonicUs();
        // 公開済みの締切は変更せず、次の1Fを一度だけ作る。
        const auto rateGoal = host_ ? 0 : periodCorrectionParts;
        rateParts_ += std::clamp<int64_t>(rateGoal-rateParts_,-timer::ClockParts,timer::ClockParts);
        auto nominal = cadence_;
        nominal.AdvanceCorrected(rateParts_,scale);
        phaseError_ = phaseShift_ = 0;
        modelReady_ = fresh && peer.modelReady;
        modelRevision_ = peer.modelRevision;
        if (fresh) {
            const auto target = peer.dueTicks + (int64_t(next_)-peer.frame) *
                (timer::ClockFrame*timer::ClockParts+peer.periodCorrectionParts)/(timer::ClockParts*scale);
            phaseError_ = target-nominal.NextTicks();
            phaseTheta_ = peer.thetaTicks; phaseRtt_ = peer.rttTicks;
            phaseShift_ = follower_.RecoveryTicks(phaseError_,next_,uint64_t(peer.stampTicks),scale);
        }
        phaseParts_ = follower_.UpdateParts(phaseError_,next_,modelReady_ && !phaseShift_,scale);
        cadence_.AdvanceCorrected(rateParts_+phaseParts_,scale);
        if (phaseShift_) {
            cadence_.ShiftTicks(phaseShift_);
            domain::session::DebugLog("[InputClock] REPHASE errorTicks=%lld shiftTicks=%lld frame=%u",
                                      phaseError_,phaseShift_,next_);
        }
        ++sampled;
    }
    PublishSchedule();
    if (sampled)
        netplay::NetplaySession::GetInstance().NotifyInputReady();
    lock.unlock();
    // 公開・送信通知の後だけ整形する。通信側とはF番号で結合し、時計の原点は実QPCで統一。
    for (unsigned i = 0; i < sendSampleCount; ++i) {
        const auto &s = sendSamples[i];
        domain::session::DebugLog("[InputCaptureSend] f=%u begin=%lld end=%lld published=%lld",
                                  s.frame, s.begin, s.end, s.published);
    }
    if (sendSampleDropped) domain::session::DebugLog("[InputCaptureSendDropped] count=%u", sendSampleDropped);
    if (stages && sampled)
        domain::session::DebugLog(
            "[CaptureStage] f=%u lock=%lld poll=%lld work=%lld published=%lld late=%lld", firstSample,
            locked - entered, pollUs, publishUs - locked, publishUs, nowUs - firstDue);
    // OSによる採取遅延を、実測した過去入力であるかのように扱わない。
    if (sampled > 1)
        domain::session::DebugLog("[InputClock] late ticks=%u held previous input", sampled - 1);
}
} // namespace cccaster::core::sync

namespace cccaster::core::sync {
int64_t InputTimeline::CapturedDeadlineUs(uint32_t frame) {
    std::lock_guard lock(mutex_);
    const auto &sample = captureTimes_[frame % captureTimes_.size()];
    return active_ && sample.frame == frame ? (sample.dueTicks + 59) / 60 : 0;
}
int64_t InputTimeline::CapturedDeadlineTicks(uint32_t frame) {
    static const bool trace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    const auto before = trace ? platform::RealMonotonicUs() : 0;
    std::lock_guard lock(mutex_);
    if (trace && platform::RealMonotonicUs() - before > 20)
        domain::session::DebugLog("[TimelineLock] f=%u wait=%lld", frame,
                                  platform::RealMonotonicUs() - before);
    const auto &sample = captureTimes_[frame % captureTimes_.size()];
    return active_ && sample.frame == frame ? sample.dueTicks : 0;
}
int64_t InputTimeline::NextDeadlineTicks() {
    std::lock_guard lock(mutex_);
    return active_ ? cadence_.NextTicks() : 0;
}
int64_t InputTimeline::NextDeadlineUs() {
    std::lock_guard lock(mutex_);
    return active_ ? cadence_.NextUs() : 0;
}
void InputTimeline::PublishSchedule() {
    auto &state = netplay::NetplaySession::GetMutableState();
    std::lock_guard lock(state.scheduleMutex);
    state.localSchedule = active_
                              ? netplay::SharedSyncState::InputSchedule{base_, next_, cadence_.NextTicks(), 0}
                              : netplay::SharedSyncState::InputSchedule{};
}
} // namespace cccaster::core::sync

namespace cccaster::core::sync {
void InputTimeline::TraceCapturedPhase(uint32_t frame) {
    CaptureTime sample;
    {
        std::lock_guard lock(mutex_);
        sample = captureTimes_[frame % captureTimes_.size()];
    }
    if (sample.frame != frame) return;
    domain::session::DebugLog("[PhaseSample] f=%u error=%lld shift=%lld theta=%lld rtt=%lld",
                                  frame, sample.error/60, sample.shift/60, sample.theta/60, sample.rtt/60);
    static const bool followTrace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
    if (followTrace)
        domain::session::DebugLog("[ClockFollow] f=%u dueTicks=%lld errorTicks=%lld phaseParts=%lld rateParts=%lld recoveryTicks=%lld ready=%d revision=%u",
            frame,sample.dueTicks,sample.error,sample.phaseParts,sample.rateParts,sample.shift,
            int(sample.ready),sample.revision);
}
}
