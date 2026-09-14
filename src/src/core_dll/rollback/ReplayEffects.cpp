#include "core_dll/rollback/ReplayEffects.hpp"
#include "core_dll/rollback/IntroSoundClock.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/mbaa_mem/CombatStress.hpp"
#include "core_dll/rollback/SoundApiProbe.hpp"
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <cstring>
#include <cstdlib>

namespace {
struct Sounds {
    uint32_t frame = 0;
    std::array<uint8_t, 1500> played{};
};
std::array<Sounds, 16> history;
std::array<uint8_t, 1500> suppressed{};
uint32_t activeFrame = 0;
bool replaying = false;
uint32_t skipped = 0;
bool soundProbe = false;
using SoundUpdate = uintptr_t (__cdecl *)();
SoundUpdate soundUpdate = nullptr;
struct SoundSample { uint32_t frame, calls, suppressed, pid, tid, serial; bool replay, warmup; int64_t ticks, begin; };
std::array<SoundSample, 32> soundSamples{};
unsigned soundSamplesUsed = 0, soundDrops = 0, soundCalls = 0;
__attribute__((force_align_arg_pointer)) uintptr_t __cdecl ProbeSoundUpdate() {
    const bool warmup = cccaster::diagnostics::sound_api::Enabled() && !cccaster::diagnostics::sound_api::attempted;
    cccaster::diagnostics::sound_api::Prepare();
    cccaster::diagnostics::sound_api::Begin(activeFrame, replaying);
    const auto beforeCalls = soundCalls, beforeSkipped = skipped;
    const auto pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    const auto started = cccaster::platform::RealMonotonicTicks();
    const auto result = soundUpdate();
    const auto elapsed = cccaster::platform::RealMonotonicTicks() - started;
    cccaster::diagnostics::sound_api::End();
    if (soundSamplesUsed < soundSamples.size())
        soundSamples[soundSamplesUsed++] = {activeFrame, soundCalls - beforeCalls,
            skipped - beforeSkipped, pid, tid, cccaster::diagnostics::sound_api::serial,
            replaying, warmup, elapsed, started};
    else ++soundDrops;
    return result;
}
} // namespace
extern "C" {
void *cccaster_sound_status_original = nullptr;
__attribute__((force_align_arg_pointer)) int __cdecl cccaster_intro_sound_status(uint32_t sound) {
    if (!activeFrame || *CC_GAME_MODE_ADDR != CC_GAME_MODE_IN_GAME ||
        (*CC_INTRO_STATE_ADDR != 1 && *CC_INTRO_STATE_ADDR != 2)) return -1;
    const bool playing = cccaster::sync::IntroSoundClock::Playing(sound, activeFrame);
    static const bool voiceTrace = std::getenv("CCCASTER_INTRO_VOICE_TRACE") != nullptr;
    if (voiceTrace && sound < 1500 && cccaster::sync::IntroSoundClock::until[sound])
        cccaster::domain::session::DebugLog("[IntroVoiceWait] frame=%u sound=%u until=%u result=%d", activeFrame,
            sound, cccaster::sync::IntroSoundClock::until[sound], playing);
    return playing;
}
// 0x4DE1E0はEAXに音源番号を受け、EAXに結果を返すゲーム固有ABI。
__attribute__((naked)) void cccaster_sound_status_hook() {
    __asm__ __volatile__(
        "pushfl\n\tpushal\n\tpushl %eax\n\tcall _cccaster_intro_sound_status\n\taddl $4,%esp\n\t"
        "testl %eax,%eax\n\tjs 1f\n\tmovl %eax,28(%esp)\n\tpopal\n\tpopfl\n\tret\n\t"
        "1: popal\n\tpopfl\n\tjmp *_cccaster_sound_status_original\n\t");
}
void *cccaster_rng_original = nullptr;
__attribute__((force_align_arg_pointer)) void __cdecl cccaster_trace_rng(uint32_t caller) {
    if (*CC_P1_NO_INPUT_FLAG_ADDR && *CC_P2_NO_INPUT_FLAG_ADDR)
        cccaster::domain::session::DebugLog("[RNGCALL] %u %08X count=%u", activeFrame, caller,
                                            *CC_RNG_STATE1_ADDR);
}
__attribute__((naked)) void cccaster_rng_hook() {
    __asm__ __volatile__("pushfl\n\tpushal\n\tpushl 36(%esp)\n\tcall _cccaster_trace_rng\n\taddl "
                         "$4,%esp\n\tpopal\n\tpopfl\n\tjmp *_cccaster_rng_original\n\t");
}
void *cccaster_sfx_original = nullptr;
uintptr_t cccaster_sfx_skip = 0x4DE223;
__attribute__((force_align_arg_pointer)) int __cdecl cccaster_sfx_should_play(uint32_t sound) {
    if (soundProbe) ++soundCalls;
    cccaster::diagnostics::sound_api::SetSound(sound);
    if (sound >= 1500)
        return 1;
    if (activeFrame && *CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME &&
        (*CC_INTRO_STATE_ADDR == 1 || *CC_INTRO_STATE_ADDR == 2)) {
        cccaster::sync::IntroSoundClock::Start(sound, activeFrame);
        static const bool voiceTrace = std::getenv("CCCASTER_INTRO_VOICE_TRACE") != nullptr;
        if (voiceTrace)
            cccaster::domain::session::DebugLog("[IntroVoice] frame=%u sound=%u duration=%u replay=%d", activeFrame,
                sound, cccaster::sync::IntroSoundClock::duration[sound], replaying);
    }
    auto &slot = history[activeFrame % history.size()];
    if (activeFrame && slot.frame == activeFrame)
        slot.played[sound] = 1;
    if (replaying && suppressed[sound]) {
        ++skipped;
        return 0;
    }
    if (replaying)
        suppressed[sound] = 1;
    return 1;
}
// このゲーム版のESIは効果音番号。元命令列はInstallで照合する。
__attribute__((naked)) void cccaster_sfx_hook() {
    __asm__ __volatile__(
        "pushfl\n\tpushal\n\tpushl %esi\n\tcall _cccaster_sfx_should_play\n\taddl $4,%esp\n\t"
        "testl %eax,%eax\n\tjz 1f\n\tpopal\n\tpopfl\n\tjmp *_cccaster_sfx_original\n\t"
        "1: popal\n\tpopfl\n\tjmp *_cccaster_sfx_skip\n\t");
}
}
namespace cccaster::sync {
bool InstallReplayEffects() {
    static bool installed = false;
    if (installed)
        return true;
    const unsigned char expected[] = {0x8b, 0x3c, 0xb5, 0xf8, 0xc6, 0x76, 0x00};
    void *address = reinterpret_cast<void *>(0x4DE210);
    if (std::memcmp(address, expected, sizeof(expected)))
        return false;
    auto result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(address, reinterpret_cast<void *>(cccaster_sfx_hook), &cccaster_sfx_original) != MH_OK)
        return false;
    if (MH_EnableHook(address) != MH_OK)
        return false;
    constexpr unsigned char expectedStatus[] = {0x57,0x8b,0x3c,0x85,0xf8,0xc6,0x76,0x00};
    auto status = reinterpret_cast<void *>(0x4de1e0);
    if (std::memcmp(status, expectedStatus, sizeof(expectedStatus)) ||
        MH_CreateHook(status, reinterpret_cast<void *>(cccaster_sound_status_hook),
                      &cccaster_sound_status_original) != MH_OK || MH_EnableHook(status) != MH_OK)
        return false;
    cccaster::testing::combat_stress::Install();
    if (std::getenv("CCCASTER_SOUND_PROBE") || cccaster::diagnostics::sound_api::Enabled()) {
        constexpr unsigned char expectedUpdate[] = {0x55,0x56,0x33,0xed,0x57,0x33,0xf6};
        auto update = reinterpret_cast<void *>(0x4de200);
        if (!std::memcmp(update, expectedUpdate, sizeof(expectedUpdate)) &&
            MH_CreateHook(update, reinterpret_cast<void *>(ProbeSoundUpdate),
                          reinterpret_cast<void **>(&soundUpdate)) == MH_OK &&
            MH_EnableHook(update) == MH_OK) soundProbe = true;
        cccaster::domain::session::DebugLog("[SoundProbeInstall] active=%d", soundProbe);
    }
    if (std::getenv("CCCASTER_TRACE_RNG")) {
        void *rng = reinterpret_cast<void *>(0x421A80);
        if (MH_CreateHook(rng, reinterpret_cast<void *>(cccaster_rng_hook), &cccaster_rng_original) !=
                MH_OK ||
            MH_EnableHook(rng) != MH_OK)
            return false;
    }
    installed = true;
    return true;
}
void BeginSimulationEffects(uint32_t frame) {
    activeFrame = frame;
    auto &slot = history[frame % history.size()];
    slot.frame = frame;
    slot.played.fill(0);
}
void BeginReplayEffects(uint32_t from, uint32_t target) {
    suppressed.fill(0);
    skipped = 0;
    for (uint32_t f = from; f < target; ++f) {
        const auto &slot = history[f % history.size()];
        if (slot.frame != f)
            continue;
        for (size_t i = 0; i < suppressed.size(); ++i)
            suppressed[i] |= slot.played[i];
    }
    replaying = true;
}
void EndReplayEffects() {
    replaying = false;
    if (cccaster::testing::IsScriptedInputEnabled())
        cccaster::domain::session::DebugLog("[Rollback] SFX suppressed=%u", skipped);
}
void FlushSoundProbe() {
    cccaster::diagnostics::sound_api::Flush();
    for (unsigned i = 0; i < soundSamplesUsed; ++i) {
        const auto &s = soundSamples[i];
        cccaster::domain::session::DebugLog("[SoundProbe] f=%u replay=%d calls=%u suppressed=%u ticks=%lld begin=%lld end=%lld pid=%u tid=%u seq=%u warmup=%d",
            s.frame, s.replay, s.calls, s.suppressed, s.ticks, s.begin, s.begin + s.ticks, s.pid, s.tid, s.serial, s.warmup);
    }
    if (soundDrops) cccaster::domain::session::DebugLog("[SoundProbeDrop] count=%u", soundDrops);
    soundSamplesUsed = soundDrops = 0;
}
} // namespace cccaster::sync
